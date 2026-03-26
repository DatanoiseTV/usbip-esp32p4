/*
 * Transfer Engine - URB Forwarding Loop
 *
 * Bridges USB/IP CMD_SUBMIT/CMD_UNLINK from the network to
 * ESP-IDF USB host transfers and sends results back.
 */

#include "transfer_engine.h"
#include "usbip_proto.h"
#include "device_manager.h"
#include "usb_host_mgr.h"
#include "event_log.h"

#include "usb/usb_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "xfer_eng";

/* Maximum transfer buffer size */
#define MAX_TRANSFER_SIZE   16384

/* Minimum transfer buffer allocation */
#define MIN_TRANSFER_SIZE   64

/* Transfer completion timeout (ms) */
#define TRANSFER_TIMEOUT_MS 10000

/* USB transfer timeout set on the transfer object (ms) */
#define USB_XFER_TIMEOUT_MS 5000

/* Linux errno values (USB/IP client expects these, not ESP-IDF values) */
#define LINUX_EIO           5
#define LINUX_ENODEV        19
#define LINUX_EPIPE         32
#define LINUX_EOVERFLOW     75
#define LINUX_ECONNRESET    104
#define LINUX_ETIMEDOUT     110

/* ------------------------------------------------------------------ */
/* Transfer callback context                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    SemaphoreHandle_t sem;
    usb_transfer_status_t status;
} xfer_ctx_t;

/**
 * @brief USB transfer completion callback.
 * Runs in USB host task context (Core 0). Only signals the semaphore.
 */
static void transfer_cb(usb_transfer_t *transfer)
{
    xfer_ctx_t *ctx = (xfer_ctx_t *)transfer->context;
    if (ctx) {
        ctx->status = transfer->status;
        xSemaphoreGive(ctx->sem);
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Map ESP-IDF USB transfer status to Linux errno.
 */
static int32_t map_usb_status(usb_transfer_status_t status)
{
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED: return 0;
    case USB_TRANSFER_STATUS_ERROR:     return -LINUX_EIO;
    case USB_TRANSFER_STATUS_TIMED_OUT: return -LINUX_ETIMEDOUT;
    case USB_TRANSFER_STATUS_CANCELED:  return -LINUX_ECONNRESET;
    case USB_TRANSFER_STATUS_STALL:     return -LINUX_EPIPE;
    case USB_TRANSFER_STATUS_NO_DEVICE: return -LINUX_ENODEV;
    case USB_TRANSFER_STATUS_OVERFLOW:  return -LINUX_EOVERFLOW;
    default:                            return -LINUX_EIO;
    }
}

/**
 * @brief Drain (discard) bytes from socket to keep the stream in sync.
 */
static int drain_socket(int fd, int nbytes)
{
    uint8_t tmp[256];
    while (nbytes > 0) {
        int chunk = nbytes > (int)sizeof(tmp) ? (int)sizeof(tmp) : nbytes;
        int ret = usbip_net_recv(fd, tmp, chunk);
        if (ret != 0) {
            return -1;
        }
        nbytes -= chunk;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* CMD_SUBMIT handler                                                 */
/* ------------------------------------------------------------------ */

static int handle_cmd_submit(int fd, usbip_header_t *hdr, const dm_device_info_t *devinfo)
{
    uint32_t seqnum   = hdr->base.seqnum;
    uint32_t ep       = hdr->base.ep;
    uint32_t direction = hdr->base.direction;
    int32_t  buflen   = hdr->u.cmd_submit.transfer_buffer_length;
    int32_t  num_iso  = hdr->u.cmd_submit.number_of_packets;

    /* Clamp buffer length */
    if (buflen < 0) buflen = 0;
    if (buflen > MAX_TRANSFER_SIZE) buflen = MAX_TRANSFER_SIZE;
    if (num_iso < 0) num_iso = 0;

    /* Determine if this is a control transfer */
    bool is_control = (ep == 0);

    /* Compute allocation size */
    size_t alloc_size = is_control ? (size_t)(buflen + 8) : (size_t)buflen;
    if (alloc_size < MIN_TRANSFER_SIZE) {
        alloc_size = MIN_TRANSFER_SIZE;
    }

    /* Allocate USB transfer */
    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(alloc_size, num_iso, &xfer);
    if (err != ESP_OK || !xfer) {
        ESP_LOGE(TAG, "Failed to allocate transfer: %s", esp_err_to_name(err));
        /* Need to drain any OUT data from the socket */
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        /* Send error reply */
        usbip_header_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.base.command   = USBIP_RET_SUBMIT;
        reply.base.seqnum    = seqnum;
        reply.base.devid     = hdr->base.devid;
        reply.base.direction = direction;
        reply.base.ep        = ep;
        reply.u.ret_submit.status = -LINUX_EIO;
        usbip_pack_header(&reply, true);
        usbip_net_send(fd, &reply, sizeof(reply));
        return 0; /* non-fatal */
    }

    /* Create completion context */
    xfer_ctx_t ctx;
    ctx.sem = xSemaphoreCreateBinary();
    if (!ctx.sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        usb_host_transfer_free(xfer);
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        usbip_header_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.base.command   = USBIP_RET_SUBMIT;
        reply.base.seqnum    = seqnum;
        reply.base.devid     = hdr->base.devid;
        reply.base.direction = direction;
        reply.base.ep        = ep;
        reply.u.ret_submit.status = -LINUX_EIO;
        usbip_pack_header(&reply, true);
        usbip_net_send(fd, &reply, sizeof(reply));
        return 0;
    }
    ctx.status = USB_TRANSFER_STATUS_ERROR;

    /* Get device handle */
    usb_device_handle_t dev_handle = usb_host_mgr_get_handle(devinfo->dev_addr);
    if (!dev_handle) {
        ESP_LOGE(TAG, "No device handle for addr %d", devinfo->dev_addr);
        vSemaphoreDelete(ctx.sem);
        usb_host_transfer_free(xfer);
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        usbip_header_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.base.command   = USBIP_RET_SUBMIT;
        reply.base.seqnum    = seqnum;
        reply.base.devid     = hdr->base.devid;
        reply.base.direction = direction;
        reply.base.ep        = ep;
        reply.u.ret_submit.status = -LINUX_ENODEV;
        usbip_pack_header(&reply, true);
        usbip_net_send(fd, &reply, sizeof(reply));
        return 0;
    }

    /* Configure the transfer */
    xfer->device_handle = dev_handle;
    xfer->callback      = transfer_cb;
    xfer->context       = &ctx;
    xfer->timeout_ms    = USB_XFER_TIMEOUT_MS;

    if (is_control) {
        /* Control transfer: copy setup packet to data_buffer[0..7] */
        memcpy(xfer->data_buffer, hdr->u.cmd_submit.setup, 8);
        xfer->num_bytes = buflen + 8;
        xfer->bEndpointAddress = 0;
    } else {
        xfer->num_bytes = buflen;
        xfer->bEndpointAddress = (uint8_t)(ep | (direction == USBIP_DIR_IN ? 0x80 : 0x00));
    }

    /* For OUT transfers: receive transfer data from socket */
    if (direction == USBIP_DIR_OUT && buflen > 0) {
        uint8_t *dest = is_control ? (xfer->data_buffer + 8) : xfer->data_buffer;
        if (usbip_net_recv(fd, dest, buflen) != 0) {
            ESP_LOGE(TAG, "Failed to recv OUT data");
            vSemaphoreDelete(ctx.sem);
            usb_host_transfer_free(xfer);
            return -1; /* socket error, fatal */
        }
    }

    /* For ISO: receive ISO packet descriptors from socket */
    if (num_iso > 0) {
        for (int i = 0; i < num_iso; i++) {
            usbip_iso_packet_descriptor_t iso_desc;
            if (usbip_net_recv(fd, &iso_desc, sizeof(iso_desc)) != 0) {
                ESP_LOGE(TAG, "Failed to recv ISO descriptors");
                vSemaphoreDelete(ctx.sem);
                usb_host_transfer_free(xfer);
                return -1;
            }
            usbip_pack_iso_descriptor(&iso_desc, false); /* network -> host */
            xfer->isoc_packet_desc[i].num_bytes = iso_desc.length;
        }
    }

    /* Submit the transfer */
    esp_err_t submit_err;
    if (is_control) {
        usb_host_client_handle_t client_hdl = usb_host_mgr_get_client_handle();
        submit_err = usb_host_transfer_submit_control(client_hdl, xfer);
    } else {
        submit_err = usb_host_transfer_submit(xfer);
    }

    int32_t reply_status = 0;
    int32_t actual_length = 0;
    int32_t reply_num_packets = 0;
    int32_t reply_error_count = 0;

    if (submit_err != ESP_OK) {
        ESP_LOGE(TAG, "Transfer submit failed: %s (ep=0x%02x)",
                 esp_err_to_name(submit_err), xfer->bEndpointAddress);
        reply_status = -LINUX_EIO;
    } else {
        /* Wait for completion */
        if (xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(TRANSFER_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Transfer timeout (seqnum=%lu), aborting ep=0x%02x",
                     (unsigned long)seqnum, xfer->bEndpointAddress);
            /*
             * Transfer is still in-flight on the USB hardware. We must abort it
             * before freeing. For non-control endpoints: halt -> flush -> clear.
             * The flush causes the callback to fire with CANCELED status.
             */
            if (!is_control && dev_handle) {
                usb_host_endpoint_halt(dev_handle, xfer->bEndpointAddress);
                usb_host_endpoint_flush(dev_handle, xfer->bEndpointAddress);
                /* Wait for the canceled callback */
                xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(1000));
                usb_host_endpoint_clear(dev_handle, xfer->bEndpointAddress);
            } else {
                /* For EP0 control: just wait longer - the USB stack's own timeout
                 * should eventually complete it. If it doesn't, we're stuck. */
                ESP_LOGW(TAG, "EP0 timeout - waiting for USB stack timeout");
                xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(6000));
            }
            reply_status = -LINUX_ETIMEDOUT;
        } else {
            /* Map USB status */
            reply_status = map_usb_status(ctx.status);
            if (reply_status == 0) {
                actual_length = xfer->actual_num_bytes;
                if (is_control && actual_length >= 8) {
                    actual_length -= 8; /* subtract setup packet */
                } else if (is_control) {
                    actual_length = 0;
                }
            }
        }

        /* Handle ISO results */
        if (num_iso > 0) {
            reply_num_packets = num_iso;
            for (int i = 0; i < num_iso; i++) {
                if (xfer->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) {
                    reply_error_count++;
                }
            }
        }
    }

    /* Build RET_SUBMIT reply */
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.base.command              = USBIP_RET_SUBMIT;
    reply.base.seqnum               = seqnum;
    reply.base.devid                = hdr->base.devid;
    reply.base.direction            = direction;
    reply.base.ep                   = ep;
    reply.u.ret_submit.status       = reply_status;
    reply.u.ret_submit.actual_length = actual_length;
    reply.u.ret_submit.start_frame  = 0;
    reply.u.ret_submit.number_of_packets = reply_num_packets;
    reply.u.ret_submit.error_count  = reply_error_count;

    usbip_pack_header(&reply, true);

    /* Send header */
    if (usbip_net_send(fd, &reply, sizeof(reply)) != 0) {
        ESP_LOGE(TAG, "Failed to send RET_SUBMIT header");
        vSemaphoreDelete(ctx.sem);
        usb_host_transfer_free(xfer);
        return -1;
    }

    /* For IN transfers with data: send the payload */
    if (direction == USBIP_DIR_IN && actual_length > 0 && reply_status == 0) {
        uint8_t *src = is_control ? (xfer->data_buffer + 8) : xfer->data_buffer;
        if (usbip_net_send(fd, src, actual_length) != 0) {
            ESP_LOGE(TAG, "Failed to send IN data");
            vSemaphoreDelete(ctx.sem);
            usb_host_transfer_free(xfer);
            return -1;
        }
    }

    /* For ISO: send ISO packet descriptors */
    if (num_iso > 0) {
        for (int i = 0; i < num_iso; i++) {
            usbip_iso_packet_descriptor_t iso_desc;
            iso_desc.offset        = 0;
            iso_desc.length        = xfer->isoc_packet_desc[i].num_bytes;
            iso_desc.actual_length = xfer->isoc_packet_desc[i].actual_num_bytes;
            iso_desc.status        = (uint32_t)map_usb_status(xfer->isoc_packet_desc[i].status);
            usbip_pack_iso_descriptor(&iso_desc, true); /* host -> network */
            if (usbip_net_send(fd, &iso_desc, sizeof(iso_desc)) != 0) {
                ESP_LOGE(TAG, "Failed to send ISO descriptors");
                vSemaphoreDelete(ctx.sem);
                usb_host_transfer_free(xfer);
                return -1;
            }
        }
    }

    /* Log every transfer at INFO level for debugging */
    if (reply_status == 0) {
        ESP_LOGI(TAG, "URB seqnum=%lu ep=0x%02x %s len=%ld ok",
                 (unsigned long)seqnum,
                 (unsigned)(ep | (direction == USBIP_DIR_IN ? 0x80 : 0x00)),
                 direction == USBIP_DIR_IN ? "IN" : "OUT",
                 (long)actual_length);
    } else {
        ESP_LOGW(TAG, "URB seqnum=%lu ep=0x%02x %s len=%ld status=%ld",
                 (unsigned long)seqnum,
                 (unsigned)(ep | (direction == USBIP_DIR_IN ? 0x80 : 0x00)),
                 direction == USBIP_DIR_IN ? "IN" : "OUT",
                 (long)actual_length,
                 (long)reply_status);
    }

    vSemaphoreDelete(ctx.sem);
    usb_host_transfer_free(xfer);
    return 0;
}

/* ------------------------------------------------------------------ */
/* CMD_UNLINK handler                                                 */
/* ------------------------------------------------------------------ */

static int handle_cmd_unlink(int fd, usbip_header_t *hdr)
{
    /*
     * Since we run transfers synchronously (one at a time),
     * there's nothing to cancel. Send RET_UNLINK with -ECONNRESET.
     */
    uint32_t seqnum = hdr->base.seqnum;

    ESP_LOGI(TAG, "CMD_UNLINK seqnum=%lu unlink_seqnum=%lu",
             (unsigned long)seqnum,
             (unsigned long)hdr->u.cmd_unlink.seqnum);

    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.base.command          = USBIP_RET_UNLINK;
    reply.base.seqnum           = seqnum;
    reply.base.devid            = hdr->base.devid;
    reply.base.direction        = 0;
    reply.base.ep               = 0;
    reply.u.ret_unlink.status   = -LINUX_ECONNRESET;

    usbip_pack_header(&reply, true);

    if (usbip_net_send(fd, &reply, sizeof(reply)) != 0) {
        ESP_LOGE(TAG, "Failed to send RET_UNLINK");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t transfer_engine_init(void)
{
    ESP_LOGI(TAG, "Transfer engine initialized");
    return ESP_OK;
}

esp_err_t transfer_engine_stop(void)
{
    ESP_LOGI(TAG, "Transfer engine stopped");
    return ESP_OK;
}

int transfer_engine_run(int sockfd, const char *busid)
{
    ESP_LOGI(TAG, "Transfer engine starting: fd=%d busid=%s", sockfd, busid ? busid : "");

    /* Look up the device */
    int dev_index = -1;
    if (device_manager_lookup(busid, &dev_index) != ESP_OK) {
        ESP_LOGE(TAG, "Device not found: %s", busid ? busid : "(null)");
        return -1;
    }

    dm_device_info_t devinfo;
    if (device_manager_get(dev_index, &devinfo) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device info for index %d", dev_index);
        return -1;
    }

    event_log_add(EVENT_LOG_LEVEL_INFO, "Transfer engine started for %s (addr=%d)",
                  busid, devinfo.dev_addr);

    /* Main URB forwarding loop */
    int ret = 0;
    while (1) {
        /* 1. Read USB/IP header (48 bytes) from socket */
        usbip_header_t hdr;
        if (usbip_net_recv(sockfd, &hdr, sizeof(hdr)) != 0) {
            ESP_LOGI(TAG, "Client disconnected or socket error");
            ret = 0; /* clean disconnect */
            break;
        }

        /* 2. Unpack from network byte order */
        usbip_pack_header(&hdr, false);

        /* 3. Dispatch by command */
        int rc;
        switch (hdr.base.command) {
        case USBIP_CMD_SUBMIT:
            rc = handle_cmd_submit(sockfd, &hdr, &devinfo);
            break;
        case USBIP_CMD_UNLINK:
            rc = handle_cmd_unlink(sockfd, &hdr);
            break;
        default:
            ESP_LOGE(TAG, "Unknown command: 0x%08lx", (unsigned long)hdr.base.command);
            rc = -1;
            break;
        }

        if (rc < 0) {
            ESP_LOGE(TAG, "Handler error, exiting loop");
            ret = -1;
            break;
        }

        /* Re-fetch device info to check if device is still present */
        if (device_manager_get(dev_index, &devinfo) != ESP_OK || !devinfo.in_use) {
            ESP_LOGW(TAG, "Device removed during transfer session");
            ret = -1;
            break;
        }
    }

    event_log_add(EVENT_LOG_LEVEL_INFO, "Transfer engine stopped for %s", busid);
    return ret;
}
