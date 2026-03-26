/*
 * Transfer Engine - Concurrent URB Forwarding Loop
 *
 * Bridges USB/IP CMD_SUBMIT/CMD_UNLINK from the network to
 * ESP-IDF USB host transfers and sends results back.
 *
 * Uses a pending URB table to support concurrent IN+OUT transfers,
 * which is required for devices like IC programmers that send a
 * command via OUT and expect an immediate response via IN.
 */

#include "transfer_engine.h"
#include "usbip_proto.h"
#include "device_manager.h"
#include "usb_host_mgr.h"
#include "event_log.h"

#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <lwip/sockets.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "xfer_eng";

/* Maximum transfer buffer size */
#define MAX_TRANSFER_SIZE   16384

/* Minimum transfer buffer allocation */
#define MIN_TRANSFER_SIZE   64

/* USB transfer timeout set on the transfer object (ms) */
#define USB_XFER_TIMEOUT_MS 30000

/* Pending URB timeout (microseconds) - 30 seconds */
#define PENDING_URB_TIMEOUT_US  (30 * 1000 * 1000LL)

/* select() timeout for polling the socket (microseconds) */
#define SELECT_TIMEOUT_US   10000

/* Maximum concurrent pending URBs */
#define MAX_PENDING_URBS    16

/* Linux errno values (USB/IP client expects these, not ESP-IDF values) */
#define LINUX_EIO           5
#define LINUX_ENODEV        19
#define LINUX_EPIPE         32
#define LINUX_EOVERFLOW     75
#define LINUX_ECONNRESET    104
#define LINUX_ETIMEDOUT     110

/* ------------------------------------------------------------------ */
/* Pending URB table                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    bool active;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    int32_t buflen;
    int32_t num_iso;
    int64_t submit_time_us;
    usb_transfer_t *xfer;
    SemaphoreHandle_t sem;
    usb_transfer_status_t usb_status;
    bool is_control;
} pending_urb_t;

static pending_urb_t s_pending[MAX_PENDING_URBS];

/* ------------------------------------------------------------------ */
/* Transfer callback                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief USB transfer completion callback.
 * Runs in USB host task context (Core 0). Only signals the semaphore.
 */
static void transfer_cb(usb_transfer_t *transfer)
{
    pending_urb_t *slot = (pending_urb_t *)transfer->context;
    if (slot) {
        slot->usb_status = transfer->status;
        xSemaphoreGive(slot->sem);
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

/**
 * @brief Find a free slot in the pending URB table.
 * @return slot index, or -1 if full
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_PENDING_URBS; i++) {
        if (!s_pending[i].active) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Find a pending URB by seqnum.
 * @return slot index, or -1 if not found
 */
static int find_pending_by_seqnum(uint32_t seqnum)
{
    for (int i = 0; i < MAX_PENDING_URBS; i++) {
        if (s_pending[i].active && s_pending[i].seqnum == seqnum) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Send an error RET_SUBMIT reply (no transfer data).
 */
static int send_error_reply(int fd, uint32_t seqnum, uint32_t devid,
                            uint32_t direction, uint32_t ep, int32_t status)
{
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.base.command   = USBIP_RET_SUBMIT;
    reply.base.seqnum    = seqnum;
    reply.base.devid     = devid;
    reply.base.direction = direction;
    reply.base.ep        = ep;
    reply.u.ret_submit.status = status;
    usbip_pack_header(&reply, true);
    return usbip_net_send(fd, &reply, sizeof(reply));
}

/**
 * @brief Abort a pending URB slot: halt/flush/clear the endpoint.
 */
static void abort_pending_urb(pending_urb_t *slot, usb_device_handle_t dev_handle)
{
    if (!slot->is_control && dev_handle) {
        uint8_t ep_addr = slot->xfer->bEndpointAddress;
        usb_host_endpoint_halt(dev_handle, ep_addr);
        usb_host_endpoint_flush(dev_handle, ep_addr);
        /* Wait for the canceled callback */
        xSemaphoreTake(slot->sem, pdMS_TO_TICKS(1000));
        usb_host_endpoint_clear(dev_handle, ep_addr);
    } else {
        /* For EP0 control: wait for USB stack's own timeout */
        ESP_LOGW(TAG, "EP0 timeout - waiting for USB stack timeout");
        xSemaphoreTake(slot->sem, pdMS_TO_TICKS(6000));
    }
}

/**
 * @brief Free resources for a pending URB slot and mark it inactive.
 */
static void free_pending_slot(pending_urb_t *slot)
{
    if (slot->sem) {
        vSemaphoreDelete(slot->sem);
        slot->sem = NULL;
    }
    if (slot->xfer) {
        usb_host_transfer_free(slot->xfer);
        slot->xfer = NULL;
    }
    slot->active = false;
}

/**
 * @brief Send RET_SUBMIT for a completed pending URB.
 * @return 0 on success, -1 on socket error
 */
static int send_completed_reply(int fd, pending_urb_t *slot)
{
    int32_t reply_status = map_usb_status(slot->usb_status);
    int32_t actual_length = 0;
    int32_t reply_num_packets = 0;
    int32_t reply_error_count = 0;

    if (reply_status == 0) {
        actual_length = slot->xfer->actual_num_bytes;
        if (slot->is_control && actual_length >= 8) {
            actual_length -= 8; /* subtract setup packet */
        } else if (slot->is_control) {
            actual_length = 0;
        }
    }

    /* Handle ISO results */
    if (slot->num_iso > 0) {
        reply_num_packets = slot->num_iso;
        for (int i = 0; i < slot->num_iso; i++) {
            if (slot->xfer->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) {
                reply_error_count++;
            }
        }
    }

    /* Build RET_SUBMIT reply */
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.base.command              = USBIP_RET_SUBMIT;
    reply.base.seqnum               = slot->seqnum;
    reply.base.devid                = slot->devid;
    reply.base.direction            = slot->direction;
    reply.base.ep                   = slot->ep;
    reply.u.ret_submit.status       = reply_status;
    reply.u.ret_submit.actual_length = actual_length;
    reply.u.ret_submit.start_frame  = 0;
    reply.u.ret_submit.number_of_packets = reply_num_packets;
    reply.u.ret_submit.error_count  = reply_error_count;

    usbip_pack_header(&reply, true);

    /* Send header */
    if (usbip_net_send(fd, &reply, sizeof(reply)) != 0) {
        ESP_LOGE(TAG, "Failed to send RET_SUBMIT header");
        return -1;
    }

    /* For IN transfers with data: send the payload */
    if (slot->direction == USBIP_DIR_IN && actual_length > 0 && reply_status == 0) {
        uint8_t *src = slot->is_control ? (slot->xfer->data_buffer + 8) : slot->xfer->data_buffer;
        if (usbip_net_send(fd, src, actual_length) != 0) {
            ESP_LOGE(TAG, "Failed to send IN data");
            return -1;
        }
    }

    /* For ISO: send ISO packet descriptors */
    if (slot->num_iso > 0) {
        for (int i = 0; i < slot->num_iso; i++) {
            usbip_iso_packet_descriptor_t iso_desc;
            iso_desc.offset        = 0;
            iso_desc.length        = slot->xfer->isoc_packet_desc[i].num_bytes;
            iso_desc.actual_length = slot->xfer->isoc_packet_desc[i].actual_num_bytes;
            iso_desc.status        = (uint32_t)map_usb_status(slot->xfer->isoc_packet_desc[i].status);
            usbip_pack_iso_descriptor(&iso_desc, true); /* host -> network */
            if (usbip_net_send(fd, &iso_desc, sizeof(iso_desc)) != 0) {
                ESP_LOGE(TAG, "Failed to send ISO descriptors");
                return -1;
            }
        }
    }

    /* Log every transfer at INFO level for debugging */
    if (reply_status == 0) {
        ESP_LOGI(TAG, "URB seqnum=%lu ep=0x%02x %s len=%ld ok",
                 (unsigned long)slot->seqnum,
                 (unsigned)(slot->ep | (slot->direction == USBIP_DIR_IN ? 0x80 : 0x00)),
                 slot->direction == USBIP_DIR_IN ? "IN" : "OUT",
                 (long)actual_length);
    } else {
        ESP_LOGW(TAG, "URB seqnum=%lu ep=0x%02x %s len=%ld status=%ld",
                 (unsigned long)slot->seqnum,
                 (unsigned)(slot->ep | (slot->direction == USBIP_DIR_IN ? 0x80 : 0x00)),
                 slot->direction == USBIP_DIR_IN ? "IN" : "OUT",
                 (long)actual_length,
                 (long)reply_status);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* CMD_SUBMIT handler (non-blocking: submit and store in pending)      */
/* ------------------------------------------------------------------ */

static int handle_cmd_submit(int fd, usbip_header_t *hdr, const dm_device_info_t *devinfo)
{
    uint32_t seqnum   = hdr->base.seqnum;
    uint32_t ep       = hdr->base.ep;
    uint32_t direction = hdr->base.direction;
    int32_t  buflen   = hdr->u.cmd_submit.transfer_buffer_length;
    int32_t  num_iso  = hdr->u.cmd_submit.number_of_packets;

    ESP_LOGI(TAG, "CMD_SUBMIT seq=%lu ep=%lu dir=%s buflen=%ld flags=0x%08lx",
             (unsigned long)seqnum, (unsigned long)ep,
             direction == USBIP_DIR_IN ? "IN" : "OUT",
             (long)buflen, (unsigned long)hdr->u.cmd_submit.transfer_flags);

    /* Clamp buffer length */
    if (buflen < 0) buflen = 0;
    if (buflen > MAX_TRANSFER_SIZE) buflen = MAX_TRANSFER_SIZE;
    if (num_iso < 0) num_iso = 0;

    /* Find a free slot */
    int slot_idx = find_free_slot();
    if (slot_idx < 0) {
        ESP_LOGE(TAG, "Pending URB table full (%d slots)", MAX_PENDING_URBS);
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        send_error_reply(fd, seqnum, hdr->base.devid, direction, ep, -LINUX_EIO);
        return 0; /* non-fatal */
    }

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
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        send_error_reply(fd, seqnum, hdr->base.devid, direction, ep, -LINUX_EIO);
        return 0; /* non-fatal */
    }

    /* Create completion semaphore */
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        usb_host_transfer_free(xfer);
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        send_error_reply(fd, seqnum, hdr->base.devid, direction, ep, -LINUX_EIO);
        return 0;
    }

    /* Initialize the pending slot (before submit, so callback can access it) */
    pending_urb_t *slot = &s_pending[slot_idx];
    slot->active        = true;
    slot->seqnum        = seqnum;
    slot->devid         = hdr->base.devid;
    slot->direction     = direction;
    slot->ep            = ep;
    slot->buflen        = buflen;
    slot->num_iso       = num_iso;
    slot->submit_time_us = esp_timer_get_time();
    slot->xfer          = xfer;
    slot->sem           = sem;
    slot->usb_status    = USB_TRANSFER_STATUS_ERROR;
    slot->is_control    = is_control;

    /* Get device handle */
    usb_device_handle_t dev_handle = usb_host_mgr_get_handle(devinfo->dev_addr);
    if (!dev_handle) {
        ESP_LOGE(TAG, "No device handle for addr %d", devinfo->dev_addr);
        if (direction == USBIP_DIR_OUT && buflen > 0) {
            drain_socket(fd, buflen);
        }
        if (num_iso > 0) {
            drain_socket(fd, num_iso * 16);
        }
        send_error_reply(fd, seqnum, hdr->base.devid, direction, ep, -LINUX_ENODEV);
        free_pending_slot(slot);
        return 0;
    }

    /* Configure the transfer */
    xfer->device_handle = dev_handle;
    xfer->callback      = transfer_cb;
    xfer->context       = slot;  /* callback context points to the pending slot */
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
            free_pending_slot(slot);
            return -1; /* socket error, fatal */
        }
    }

    /* For ISO: receive ISO packet descriptors from socket */
    if (num_iso > 0) {
        for (int i = 0; i < num_iso; i++) {
            usbip_iso_packet_descriptor_t iso_desc;
            if (usbip_net_recv(fd, &iso_desc, sizeof(iso_desc)) != 0) {
                ESP_LOGE(TAG, "Failed to recv ISO descriptors");
                free_pending_slot(slot);
                return -1;
            }
            usbip_pack_iso_descriptor(&iso_desc, false); /* network -> host */
            xfer->isoc_packet_desc[i].num_bytes = iso_desc.length;
        }
    }

    /* Submit the transfer */
    ESP_LOGI(TAG, "Submitting: ep_addr=0x%02x num_bytes=%d timeout=%dms",
             xfer->bEndpointAddress, xfer->num_bytes, xfer->timeout_ms);
    esp_err_t submit_err;
    if (is_control) {
        usb_host_client_handle_t client_hdl = usb_host_mgr_get_client_handle();
        submit_err = usb_host_transfer_submit_control(client_hdl, xfer);
    } else {
        submit_err = usb_host_transfer_submit(xfer);
    }

    if (submit_err != ESP_OK) {
        ESP_LOGE(TAG, "Transfer submit failed: %s (ep=0x%02x)",
                 esp_err_to_name(submit_err), xfer->bEndpointAddress);
        send_error_reply(fd, seqnum, hdr->base.devid, direction, ep, -LINUX_EIO);
        free_pending_slot(slot);
        return 0; /* non-fatal */
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* CMD_UNLINK handler                                                  */
/* ------------------------------------------------------------------ */

static int handle_cmd_unlink(int fd, usbip_header_t *hdr, const dm_device_info_t *devinfo)
{
    uint32_t seqnum = hdr->base.seqnum;
    uint32_t unlink_seqnum = hdr->u.cmd_unlink.seqnum;

    ESP_LOGI(TAG, "CMD_UNLINK seqnum=%lu unlink_seqnum=%lu",
             (unsigned long)seqnum,
             (unsigned long)unlink_seqnum);

    int slot_idx = find_pending_by_seqnum(unlink_seqnum);

    if (slot_idx >= 0) {
        /* Found the pending URB - abort it */
        pending_urb_t *slot = &s_pending[slot_idx];
        usb_device_handle_t dev_handle = usb_host_mgr_get_handle(devinfo->dev_addr);

        ESP_LOGI(TAG, "Aborting pending URB seqnum=%lu ep=0x%02x",
                 (unsigned long)unlink_seqnum,
                 (unsigned)(slot->ep | (slot->direction == USBIP_DIR_IN ? 0x80 : 0x00)));

        abort_pending_urb(slot, dev_handle);

        /* Send RET_SUBMIT with -ECONNRESET for the original URB */
        send_error_reply(fd, slot->seqnum, slot->devid, slot->direction, slot->ep, -LINUX_ECONNRESET);
        free_pending_slot(slot);
    }

    /* Send RET_UNLINK response */
    usbip_header_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.base.command          = USBIP_RET_UNLINK;
    reply.base.seqnum           = seqnum;
    reply.base.devid            = hdr->base.devid;
    reply.base.direction        = 0;
    reply.base.ep               = 0;
    reply.u.ret_unlink.status   = (slot_idx >= 0) ? 0 : -LINUX_ECONNRESET;

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

    /* Initialize pending URB table */
    memset(s_pending, 0, sizeof(s_pending));

    /* Main concurrent URB forwarding loop */
    int ret = 0;
    while (1) {
        /* ---- Phase 1: Check socket for new data (non-blocking) ---- */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = SELECT_TIMEOUT_US;

        int sel_ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);

        if (sel_ret < 0) {
            ESP_LOGE(TAG, "select() error: errno=%d", errno);
            ret = -1;
            break;
        }

        if (sel_ret > 0 && FD_ISSET(sockfd, &readfds)) {
            /* Socket has data - read USB/IP header */
            usbip_header_t hdr;
            if (usbip_net_recv(sockfd, &hdr, sizeof(hdr)) != 0) {
                ESP_LOGI(TAG, "Client disconnected or socket error");
                ret = 0; /* clean disconnect */
                break;
            }

            /* Unpack from network byte order */
            usbip_pack_header(&hdr, false);

            /* Dispatch by command */
            int rc;
            switch (hdr.base.command) {
            case USBIP_CMD_SUBMIT:
                rc = handle_cmd_submit(sockfd, &hdr, &devinfo);
                break;
            case USBIP_CMD_UNLINK:
                rc = handle_cmd_unlink(sockfd, &hdr, &devinfo);
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
        }

        /* ---- Phase 2: Check all pending URBs for completions ---- */
        int64_t now = esp_timer_get_time();
        usb_device_handle_t dev_handle = usb_host_mgr_get_handle(devinfo.dev_addr);

        for (int i = 0; i < MAX_PENDING_URBS; i++) {
            pending_urb_t *slot = &s_pending[i];
            if (!slot->active) {
                continue;
            }

            if (xSemaphoreTake(slot->sem, 0) == pdTRUE) {
                /* Transfer completed! */
                if (send_completed_reply(sockfd, slot) != 0) {
                    free_pending_slot(slot);
                    ret = -1;
                    goto cleanup;
                }
                free_pending_slot(slot);
            } else if ((now - slot->submit_time_us) > PENDING_URB_TIMEOUT_US) {
                /* Timeout - abort */
                ESP_LOGE(TAG, "Transfer timeout (seqnum=%lu), aborting ep=0x%02x",
                         (unsigned long)slot->seqnum, slot->xfer->bEndpointAddress);

                abort_pending_urb(slot, dev_handle);

                /* Send timeout reply */
                send_error_reply(sockfd, slot->seqnum, slot->devid,
                                slot->direction, slot->ep, -LINUX_ETIMEDOUT);
                free_pending_slot(slot);
            }
        }

        /* ---- Phase 3: Check if device still exists ---- */
        if (device_manager_get(dev_index, &devinfo) != ESP_OK || !devinfo.in_use) {
            ESP_LOGW(TAG, "Device removed during transfer session");
            ret = -1;
            break;
        }
    }

cleanup:
    /* Abort all active pending URBs */
    {
        usb_device_handle_t dev_handle = usb_host_mgr_get_handle(devinfo.dev_addr);
        for (int i = 0; i < MAX_PENDING_URBS; i++) {
            pending_urb_t *slot = &s_pending[i];
            if (!slot->active) {
                continue;
            }
            ESP_LOGW(TAG, "Cleanup: aborting pending URB seqnum=%lu",
                     (unsigned long)slot->seqnum);
            abort_pending_urb(slot, dev_handle);
            free_pending_slot(slot);
        }
    }

    event_log_add(EVENT_LOG_LEVEL_INFO, "Transfer engine stopped for %s", busid);
    return ret;
}
