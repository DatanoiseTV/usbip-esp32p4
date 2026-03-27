/*
 * USB/IP Connection Handler
 * Handles per-client USB/IP protocol: DEVLIST and IMPORT operations.
 */

#include "usbip_server.h"
#include "usbip_proto.h"
#include "device_manager.h"
#include "transfer_engine.h"
#include "usb_host_mgr.h"
#include "event_log.h"
#include "esp_log.h"

#include <string.h>
#include <lwip/sockets.h>

static const char *TAG = "usbip_conn";

/* Speed mapping from device_manager enum to USB/IP wire values */
static uint32_t dm_speed_to_usbip(device_speed_t speed)
{
    switch (speed) {
        case DEV_SPEED_LOW:   return 1; /* USB_SPEED_LOW */
        case DEV_SPEED_FULL:  return 2; /* USB_SPEED_FULL */
        case DEV_SPEED_HIGH:  return 3; /* USB_SPEED_HIGH */
        case DEV_SPEED_SUPER: return 5; /* USB_SPEED_SUPER */
        default:              return 2;
    }
}

/* Fill a usbip_usb_device_t from dm_device_info_t */
static void fill_usb_device(usbip_usb_device_t *udev, const dm_device_info_t *info)
{
    memset(udev, 0, sizeof(*udev));

    snprintf(udev->path, sizeof(udev->path), "/usb/device/%s", info->path);
    strncpy(udev->busid, info->path, sizeof(udev->busid) - 1);
    udev->busid[sizeof(udev->busid) - 1] = '\0';

    udev->busnum  = info->bus_id;
    udev->devnum  = info->dev_addr;
    udev->speed   = dm_speed_to_usbip(info->speed);

    udev->idVendor   = info->vendor_id;
    udev->idProduct  = info->product_id;
    udev->bcdDevice  = info->bcd_device;

    udev->bDeviceClass      = info->dev_class;
    udev->bDeviceSubClass   = info->dev_subclass;
    udev->bDeviceProtocol   = info->dev_protocol;
    udev->bConfigurationValue = 1;
    udev->bNumConfigurations  = info->num_configurations;
    udev->bNumInterfaces      = info->num_interfaces > 0 ? info->num_interfaces : 1;
}

/* Format IPv4 address for logging */
static void ip4_to_str(uint32_t ip_nbo, char *buf, size_t buflen)
{
    const uint8_t *b = (const uint8_t *)&ip_nbo;
    snprintf(buf, buflen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* --- DEVLIST handler --- */

/**
 * Snapshot callback: collects device info into an array under lock.
 * This ensures ndev exactly matches the device descriptors sent.
 */
typedef struct {
    dm_device_info_t devices[CONFIG_USBIP_MAX_DEVICES];
    int count;
} devlist_snapshot_t;

static bool devlist_snapshot_cb(int index, const dm_device_info_t *info, void *user_data)
{
    devlist_snapshot_t *snap = (devlist_snapshot_t *)user_data;
    if (snap->count < CONFIG_USBIP_MAX_DEVICES) {
        snap->devices[snap->count] = *info;
        snap->count++;
    }
    return true;
}

static int handle_devlist(int fd, uint16_t client_version)
{
    /* Collect a snapshot of all devices atomically (foreach holds the lock) */
    devlist_snapshot_t snap;
    snap.count = 0;
    device_manager_foreach(devlist_snapshot_cb, &snap);

    /* Send OP_REP_DEVLIST header */
    usbip_op_common_t reply_hdr = {
        .version = client_version,
        .code    = OP_REP_DEVLIST,
        .status  = ST_OK,
    };
    usbip_pack_op_common(&reply_hdr, true);

    if (usbip_net_send(fd, &reply_hdr, sizeof(reply_hdr)) < 0) {
        return -1;
    }

    /* Send device count - guaranteed to match what follows (atomic snapshot) */
    usbip_op_devlist_reply_t reply_body = {
        .ndev = (uint32_t)snap.count,
    };
    usbip_pack_devlist_reply(&reply_body, true);

    if (usbip_net_send(fd, &reply_body, sizeof(reply_body)) < 0) {
        return -1;
    }

    /* Send each device descriptor + interface descriptors */
    for (int d = 0; d < snap.count; d++) {
        const dm_device_info_t *info = &snap.devices[d];

        usbip_usb_device_t udev;
        fill_usb_device(&udev, info);
        usbip_pack_usb_device(&udev, true);

        if (usbip_net_send(fd, &udev, sizeof(udev)) < 0) {
            return -1;
        }

        /* Send bNumInterfaces interface descriptors (spec requires exactly this many) */
        int num_ifaces = info->num_interfaces > 0 ? info->num_interfaces : 1;
        for (int i = 0; i < num_ifaces; i++) {
            usbip_usb_interface_t iface = {
                .bInterfaceClass    = info->interfaces[i].bInterfaceClass,
                .bInterfaceSubClass = info->interfaces[i].bInterfaceSubClass,
                .bInterfaceProtocol = info->interfaces[i].bInterfaceProtocol,
                .padding            = 0,
            };

            if (usbip_net_send(fd, &iface, sizeof(iface)) < 0) {
                return -1;
            }
        }
    }

    return 0;
}

/* --- IMPORT handler --- */

static int handle_import(int fd, uint16_t client_version, uint32_t client_ip)
{
    /* Read the import request (busid) */
    usbip_op_import_request_t req;
    if (usbip_net_recv(fd, &req, sizeof(req)) < 0) {
        return -1;
    }

    /* Ensure null termination */
    req.busid[USBIP_BUSID_MAX - 1] = '\0';

    ESP_LOGI(TAG, "Import request for busid='%s'", req.busid);

    /* Look up device by busid (path) */
    int dev_index = -1;
    esp_err_t err = device_manager_lookup(req.busid, &dev_index);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device '%s' not found", req.busid);

        /* Send failure reply */
        usbip_op_common_t reply_hdr = {
            .version = client_version,
            .code    = OP_REP_IMPORT,
            .status  = ST_NA,
        };
        usbip_pack_op_common(&reply_hdr, true);
        usbip_net_send(fd, &reply_hdr, sizeof(reply_hdr));
        return 0; /* Not fatal - client can retry or send another op */
    }

    /* Try to import (claim) the device */
    err = device_manager_import(dev_index, client_ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to import device '%s': %s", req.busid, esp_err_to_name(err));

        usbip_op_common_t reply_hdr = {
            .version = client_version,
            .code    = OP_REP_IMPORT,
            .status  = ST_NA,
        };
        usbip_pack_op_common(&reply_hdr, true);
        usbip_net_send(fd, &reply_hdr, sizeof(reply_hdr));
        return 0;
    }

    /* Get device info for the reply */
    dm_device_info_t info;
    err = device_manager_get(dev_index, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device info after import");
        device_manager_release(dev_index);
        return -1;
    }

    /* Send success reply header */
    usbip_op_common_t reply_hdr = {
        .version = client_version,
        .code    = OP_REP_IMPORT,
        .status  = ST_OK,
    };
    usbip_pack_op_common(&reply_hdr, true);

    if (usbip_net_send(fd, &reply_hdr, sizeof(reply_hdr)) < 0) {
        device_manager_release(dev_index);
        return -1;
    }

    /* Send the device descriptor.
     * Per usbip-win2: busid in reply MUST exactly match the request busid
     * (strncmp validation on client side). */
    usbip_usb_device_t udev;
    fill_usb_device(&udev, &info);
    /* Overwrite busid to guarantee exact match with request */
    memset(udev.busid, 0, sizeof(udev.busid));
    strncpy(udev.busid, req.busid, sizeof(udev.busid) - 1);
    usbip_pack_usb_device(&udev, true);

    if (usbip_net_send(fd, &udev, sizeof(udev)) < 0) {
        device_manager_release(dev_index);
        return -1;
    }

    ESP_LOGI(TAG, "Device '%s' imported successfully, entering transfer loop", req.busid);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s imported", req.busid);

    /* Claim all interfaces before non-control transfers */
    usb_host_mgr_claim_interfaces(info.dev_addr);

    /* Enter the URB transfer loop (blocks until done) */
    transfer_engine_run(fd, req.busid);

    /* Release interfaces and the device */
    usb_host_mgr_release_interfaces(info.dev_addr);
    device_manager_release(dev_index);
    ESP_LOGI(TAG, "Transfer loop ended for device '%s'", req.busid);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s released (transfer ended)", req.busid);

    return -1; /* Signal caller to exit (connection is done after import+transfer) */
}

/* --- Main connection handler --- */

void usbip_connection_handle(void *arg)
{
    usbip_conn_ctx_t *ctx = (usbip_conn_ctx_t *)arg;
    int fd = ctx->fd;
    uint32_t client_ip = ctx->client_ip;
    free(ctx); /* Handler owns and frees the context */

    char ip_str[16];
    ip4_to_str(client_ip, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Connection handler started for %s (fd=%d)", ip_str, fd);

    uint16_t client_version = USBIP_VERSION; /* Default; updated on first op */

    /* Main protocol loop */
    while (1) {
        /* Read the common header (8 bytes) */
        usbip_op_common_t op;
        if (usbip_net_recv(fd, &op, sizeof(op)) < 0) {
            ESP_LOGI(TAG, "Client %s disconnected (fd=%d)", ip_str, fd);
            break;
        }

        /* Unpack from network byte order */
        usbip_pack_op_common(&op, false);

        /* Version check */
        if (!usbip_version_supported(op.version)) {
            ESP_LOGW(TAG, "Unsupported protocol version 0x%04x from %s", op.version, ip_str);
            break;
        }

        /* Store client version to echo back in replies */
        client_version = op.version;

        /* Dispatch by op code */
        int rc;
        switch (op.code) {
            case OP_REQ_DEVLIST:
                ESP_LOGI(TAG, "OP_REQ_DEVLIST from %s", ip_str);
                rc = handle_devlist(fd, client_version);
                if (rc < 0) {
                    goto done;
                }
                break;

            case OP_REQ_IMPORT:
                ESP_LOGI(TAG, "OP_REQ_IMPORT from %s", ip_str);
                rc = handle_import(fd, client_version, client_ip);
                if (rc < 0) {
                    goto done;
                }
                break;

            default:
                ESP_LOGW(TAG, "Unknown op code 0x%04x from %s", op.code, ip_str);
                goto done;
        }
    }

done:
    /* Safety: release any devices this client may have imported */
    device_manager_release_by_ip(client_ip);
    close(fd);
    ESP_LOGI(TAG, "Connection handler exiting for %s (fd=%d)", ip_str, fd);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Client disconnected: %s", ip_str);
    vTaskDelete(NULL);
}
