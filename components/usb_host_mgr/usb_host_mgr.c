/*
 * USB Host Manager
 *
 * Two FreeRTOS tasks:
 *   - usb_host_daemon_task: installs USB host lib, pumps usb_host_lib_handle_events()
 *   - usb_class_driver_task: registers as client, pumps usb_host_client_handle_events()
 *
 * On USB_HOST_CLIENT_EVENT_NEW_DEV  -> enumerate and add to device_manager
 * On USB_HOST_CLIENT_EVENT_DEV_GONE -> remove from device_manager and close
 */

#include "usb_host_mgr.h"
#include "device_manager.h"
#include "network_mgr.h"
#include "webui.h"
#include "event_log.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "usb_host";

/* ---- Internal device tracking ---- */
#define MAX_TRACKED_DEVICES  DEVICE_MANAGER_MAX_DEVICES

typedef struct {
    bool in_use;
    bool needs_strings;  /* Deferred: read string descriptors outside event callback */
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
} tracked_device_t;

static tracked_device_t s_tracked[MAX_TRACKED_DEVICES];

/* ---- Task handles and synchronisation ---- */
static TaskHandle_t s_daemon_task_hdl;
static TaskHandle_t s_class_driver_task_hdl;
static SemaphoreHandle_t s_daemon_ready_sem;
static usb_host_client_handle_t s_client_hdl;
static volatile bool s_stop_requested;

/* Device-removal flag: set by handle_device_gone(), read by transfer engine */
static volatile uint8_t s_removed_addr = 0;

/* ---- Speed mapping: ESP-IDF usb_speed_t -> device_manager device_speed_t ---- */
static device_speed_t map_speed(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:  return DEV_SPEED_LOW;
    case USB_SPEED_FULL: return DEV_SPEED_FULL;
    case USB_SPEED_HIGH: return DEV_SPEED_HIGH;
    default:             return DEV_SPEED_FULL;
    }
}

/* ---- Tracked-device helpers ---- */

static int tracked_add(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (!s_tracked[i].in_use) {
            s_tracked[i].in_use       = true;
            s_tracked[i].needs_strings = true;
            s_tracked[i].dev_addr     = dev_addr;
            s_tracked[i].dev_hdl      = dev_hdl;
            return i;
        }
    }
    return -1;
}

static int tracked_find_by_handle(usb_device_handle_t dev_hdl)
{
    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (s_tracked[i].in_use && s_tracked[i].dev_hdl == dev_hdl) {
            return i;
        }
    }
    return -1;
}

static int tracked_find_by_addr(uint8_t dev_addr)
{
    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (s_tracked[i].in_use && s_tracked[i].dev_addr == dev_addr) {
            return i;
        }
    }
    return -1;
}

static void tracked_remove(int idx)
{
    if (idx >= 0 && idx < MAX_TRACKED_DEVICES) {
        s_tracked[idx].in_use = false;
    }
}

/* ---- String descriptor helper ---- */

static void xfer_done_cb(usb_transfer_t *transfer)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)transfer->context;
    xSemaphoreGive(sem);
}

/**
 * Read a USB string descriptor and convert UTF-16LE to ASCII.
 * @param dev_hdl   Opened device handle
 * @param str_idx   String descriptor index (0 = skip)
 * @param out       Output buffer
 * @param out_len   Size of output buffer
 */
static void read_string_descriptor(usb_device_handle_t dev_hdl, uint8_t str_idx,
                                   char *out, size_t out_len)
{
    if (str_idx == 0 || out_len == 0) {
        return;
    }

    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(64 + sizeof(usb_setup_packet_t), 0, &xfer);
    if (err != ESP_OK || !xfer) {
        ESP_LOGW(TAG, "Failed to alloc transfer for string desc: %s", esp_err_to_name(err));
        return;
    }

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        usb_host_transfer_free(xfer);
        return;
    }

    /* Fill setup packet using ESP-IDF macro */
    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    USB_SETUP_PACKET_INIT_GET_STR_DESC(setup, str_idx, 0x0409, 64);

    xfer->num_bytes = sizeof(usb_setup_packet_t) + 64;
    xfer->device_handle = dev_hdl;
    xfer->bEndpointAddress = 0x00;
    xfer->callback = xfer_done_cb;
    xfer->context = sem;
    xfer->timeout_ms = 1000;

    err = usb_host_transfer_submit_control(s_client_hdl, xfer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to submit string desc request (idx=%d): %s", str_idx, esp_err_to_name(err));
        vSemaphoreDelete(sem);
        usb_host_transfer_free(xfer);
        return;
    }

    /* Wait for completion */
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timeout reading string descriptor idx=%d", str_idx);
        vSemaphoreDelete(sem);
        usb_host_transfer_free(xfer);
        return;
    }

    vSemaphoreDelete(sem);

    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "String descriptor transfer failed (idx=%d), status=%d", str_idx, xfer->status);
        usb_host_transfer_free(xfer);
        return;
    }

    /* Parse: data starts after 8-byte setup packet
     * Byte 0 = bLength, Byte 1 = bDescriptorType (0x03), Bytes 2+ = UTF-16LE */
    uint8_t *desc = xfer->data_buffer + sizeof(usb_setup_packet_t);
    int actual_data = xfer->actual_num_bytes - sizeof(usb_setup_packet_t);
    if (actual_data < 2 || desc[1] != 0x03) {
        ESP_LOGW(TAG, "Invalid string descriptor (idx=%d)", str_idx);
        usb_host_transfer_free(xfer);
        return;
    }

    int bLength = desc[0];
    if (bLength > actual_data) {
        bLength = actual_data;
    }

    /* Convert UTF-16LE to ASCII: take low byte of each 16-bit code unit */
    int str_bytes = bLength - 2;  /* exclude bLength and bDescriptorType */
    int chars = str_bytes / 2;
    if (chars > (int)(out_len - 1)) {
        chars = (int)(out_len - 1);
    }
    for (int i = 0; i < chars; i++) {
        out[i] = (char)desc[2 + i * 2];
    }
    out[chars] = '\0';

    ESP_LOGI(TAG, "String descriptor idx=%d: \"%s\"", str_idx, out);
    usb_host_transfer_free(xfer);
}

/* ---- Device enumeration on NEW_DEV ---- */

static void handle_new_device(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl = NULL;
    esp_err_t err;

    /* 1. Open the device */
    err = usb_host_device_open(s_client_hdl, dev_addr, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open device addr %d: %s", dev_addr, esp_err_to_name(err));
        return;
    }

    /* 2. Get device info (speed, address, etc.) */
    usb_device_info_t dev_info;
    err = usb_host_device_info(dev_hdl, &dev_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device info addr %d: %s", dev_addr, esp_err_to_name(err));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    /* 3. Get device descriptor */
    const usb_device_desc_t *dev_desc = NULL;
    err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device descriptor addr %d: %s", dev_addr, esp_err_to_name(err));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    /* 4. Get active configuration descriptor */
    const usb_config_desc_t *config_desc = NULL;
    err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get config descriptor addr %d: %s", dev_addr, esp_err_to_name(err));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    ESP_LOGI(TAG, "New device: addr=%d, VID=0x%04x, PID=0x%04x, speed=%d",
             dev_addr, dev_desc->idVendor, dev_desc->idProduct, dev_info.speed);

    /* NOTE: String descriptors CANNOT be read here because this runs inside
     * the USB host client event callback (client_event_cb -> handle_new_device).
     * Submitting control transfers from within the callback re-enters the USB
     * host library and causes a crash. String descriptors will be read later
     * in a deferred context after the device is fully registered. */
    char str_manufacturer[64] = {0};
    char str_product[64] = {0};
    char str_serial[64] = {0};

    /* 5. Track the device handle internally */
    if (tracked_add(dev_addr, dev_hdl) < 0) {
        ESP_LOGE(TAG, "No tracking slot for device addr %d", dev_addr);
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    /* 6. Build busid as "1-{dev_addr}" and populate dm_device_info_t */
    dm_device_info_t dm_info = {0};
    dm_info.bus_id = 1;
    dm_info.dev_addr = dev_addr;
    dm_info.speed = map_speed(dev_info.speed);
    snprintf(dm_info.path, sizeof(dm_info.path), "1-%d", dev_addr);

    /* Fill descriptor fields */
    dm_info.vendor_id          = dev_desc->idVendor;
    dm_info.product_id         = dev_desc->idProduct;
    dm_info.bcd_device         = dev_desc->bcdDevice;
    dm_info.dev_class          = dev_desc->bDeviceClass;
    dm_info.dev_subclass       = dev_desc->bDeviceSubClass;
    dm_info.dev_protocol       = dev_desc->bDeviceProtocol;
    dm_info.num_configurations = dev_desc->bNumConfigurations;

    /* Copy string descriptors */
    strncpy(dm_info.manufacturer, str_manufacturer, sizeof(dm_info.manufacturer) - 1);
    strncpy(dm_info.product, str_product, sizeof(dm_info.product) - 1);
    strncpy(dm_info.serial, str_serial, sizeof(dm_info.serial) - 1);

    /* Allocate PSRAM and copy raw config descriptor */
    if (config_desc) {
        dm_info.config_desc_raw = heap_caps_malloc(config_desc->wTotalLength, MALLOC_CAP_SPIRAM);
        if (dm_info.config_desc_raw) {
            memcpy(dm_info.config_desc_raw, config_desc, config_desc->wTotalLength);
            dm_info.config_desc_len = config_desc->wTotalLength;
        } else {
            ESP_LOGW(TAG, "Failed to allocate PSRAM for config descriptor");
        }
    }

    /* 7. Populate interface descriptors from config descriptor */
    if (config_desc) {
        int offset = 0;
        int iface_count = 0;
        const usb_standard_desc_t *cur = (const usb_standard_desc_t *)config_desc;
        while ((cur = usb_parse_next_descriptor_of_type(
                    cur, config_desc->wTotalLength,
                    USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
            /* Only record alternate setting 0 for each interface */
            if (intf->bAlternateSetting == 0 && iface_count < DEVICE_MANAGER_MAX_INTERFACES) {
                dm_info.interfaces[iface_count].bInterfaceClass    = intf->bInterfaceClass;
                dm_info.interfaces[iface_count].bInterfaceSubClass = intf->bInterfaceSubClass;
                dm_info.interfaces[iface_count].bInterfaceProtocol = intf->bInterfaceProtocol;
                iface_count++;
            }
            ESP_LOGI(TAG, "  Interface %d: class=%d subclass=%d protocol=%d endpoints=%d",
                     intf->bInterfaceNumber, intf->bInterfaceClass,
                     intf->bInterfaceSubClass, intf->bInterfaceProtocol,
                     intf->bNumEndpoints);
        }
        dm_info.num_interfaces = iface_count > 0 ? iface_count : 1;
    }

    /* 8. Add to device_manager */
    int dm_idx = -1;
    err = device_manager_add(&dm_info, &dm_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device to device_manager: %s", esp_err_to_name(err));
        /* Still keep it tracked so we can close it on disconnect */
    } else {
        ESP_LOGI(TAG, "Device added to device_manager at index %d, busid=%s", dm_idx, dm_info.path);
    }

    event_log_add(EVENT_LOG_LEVEL_INFO, "USB device connected: addr=%d VID=%04x PID=%04x",
                  dev_addr, dev_desc->idVendor, dev_desc->idProduct);

    /* 9. Cross-component notifications */
    network_mgr_update_mdns_devices(device_manager_get_count());
    webui_notify_device_change();
}

/* ---- Device disconnect on DEV_GONE ---- */

static void handle_device_gone(usb_device_handle_t dev_hdl)
{
    int idx = tracked_find_by_handle(dev_hdl);
    if (idx < 0) {
        ESP_LOGW(TAG, "DEV_GONE for unknown device handle");
        return;
    }

    uint8_t dev_addr = s_tracked[idx].dev_addr;
    ESP_LOGI(TAG, "Device gone: addr=%d", dev_addr);
    event_log_add(EVENT_LOG_LEVEL_INFO, "USB device disconnected: addr=%d", dev_addr);

    /* Notify transfer engine about removal before any cleanup */
    usb_host_mgr_notify_removal(dev_addr);

    /* Remove from device_manager by busid */
    char busid[32];
    snprintf(busid, sizeof(busid), "1-%d", dev_addr);
    int dm_idx;
    if (device_manager_lookup(busid, &dm_idx) == ESP_OK) {
        device_manager_remove(dm_idx);
    }

    /* Close the USB device */
    usb_host_device_close(s_client_hdl, dev_hdl);
    tracked_remove(idx);

    /* Cross-component notifications */
    network_mgr_update_mdns_devices(device_manager_get_count());
    webui_notify_device_change();
}

/* ---- Client event callback ---- */

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        handle_new_device(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        handle_device_gone(event_msg->dev_gone.dev_hdl);
        break;
    default:
        break;
    }
}

/* ---- Daemon task ---- */

static void usb_host_daemon_task(void *arg)
{
    ESP_LOGI(TAG, "USB host daemon task started");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = NULL,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(err));
        xSemaphoreGive(s_daemon_ready_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB host library installed");
    xSemaphoreGive(s_daemon_ready_sem);

    while (!s_stop_requested) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (err == ESP_OK) {
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGD(TAG, "No more clients");
            }
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                ESP_LOGD(TAG, "All devices freed");
            }
        }
    }

    ESP_LOGI(TAG, "USB host daemon task stopping");

    /* Free all devices and uninstall */
    usb_host_device_free_all();
    /* Pump remaining events to let cleanup complete */
    for (int i = 0; i < 10; i++) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(50), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }
    usb_host_uninstall();
    ESP_LOGI(TAG, "USB host library uninstalled");

    vTaskDelete(NULL);
}

/* ---- Class driver task ---- */

static void usb_class_driver_task(void *arg)
{
    ESP_LOGI(TAG, "Class driver task waiting for daemon...");
    xSemaphoreTake(s_daemon_ready_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "Class driver task started");

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t err = usb_host_client_register(&client_config, &s_client_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register USB host client: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB host client registered");

    while (!s_stop_requested) {
        err = usb_host_client_handle_events(s_client_hdl, pdMS_TO_TICKS(100));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "client_handle_events: %s", esp_err_to_name(err));
        }

        /*
         * NOTE: String descriptor reading is disabled because ESP-IDF's USB host
         * stack processes transfer completions inside usb_host_client_handle_events().
         * Since this task is the one calling that function, submitting a synchronous
         * control transfer here deadlocks: we block on a semaphore waiting for the
         * completion callback, but the callback can only fire inside
         * usb_host_client_handle_events() which we're not calling because we're blocked.
         *
         * String descriptors would require a separate dedicated task with its own
         * USB host client registration. For now, devices are identified by VID:PID.
         */
    }

    ESP_LOGI(TAG, "Class driver task stopping");

    /* Close all tracked devices before deregistering */
    for (int i = 0; i < MAX_TRACKED_DEVICES; i++) {
        if (s_tracked[i].in_use) {
            usb_host_device_close(s_client_hdl, s_tracked[i].dev_hdl);
            s_tracked[i].in_use = false;
        }
    }

    usb_host_client_deregister(s_client_hdl);
    s_client_hdl = NULL;
    ESP_LOGI(TAG, "USB host client deregistered");

    vTaskDelete(NULL);
}

/* ---- Public API ---- */

esp_err_t usb_host_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing USB host manager");

    memset(s_tracked, 0, sizeof(s_tracked));
    s_stop_requested = false;
    s_client_hdl = NULL;

    s_daemon_ready_sem = xSemaphoreCreateBinary();
    if (!s_daemon_ready_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret;
    ret = xTaskCreatePinnedToCore(usb_host_daemon_task, "usb_daemon",
                                  4096, NULL, 20, &s_daemon_task_hdl, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create daemon task");
        vSemaphoreDelete(s_daemon_ready_sem);
        return ESP_ERR_NO_MEM;
    }

    ret = xTaskCreatePinnedToCore(usb_class_driver_task, "usb_class",
                                  4096, NULL, 19, &s_class_driver_task_hdl, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create class driver task");
        s_stop_requested = true;
        vSemaphoreDelete(s_daemon_ready_sem);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB host manager initialized");
    return ESP_OK;
}

void usb_host_mgr_stop(void)
{
    ESP_LOGI(TAG, "Stopping USB host manager");
    s_stop_requested = true;

    /* Unblock the class driver and daemon */
    if (s_client_hdl) {
        usb_host_client_unblock(s_client_hdl);
    }
    usb_host_lib_unblock();

    /* Wait for tasks to finish */
    vTaskDelay(pdMS_TO_TICKS(500));

    if (s_daemon_ready_sem) {
        vSemaphoreDelete(s_daemon_ready_sem);
        s_daemon_ready_sem = NULL;
    }

    ESP_LOGI(TAG, "USB host manager stopped");
}

usb_device_handle_t usb_host_mgr_get_handle(uint8_t dev_addr)
{
    int idx = tracked_find_by_addr(dev_addr);
    if (idx >= 0) {
        return s_tracked[idx].dev_hdl;
    }
    return NULL;
}

usb_host_client_handle_t usb_host_mgr_get_client_handle(void)
{
    return s_client_hdl;
}

esp_err_t usb_host_mgr_claim_interfaces(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl = usb_host_mgr_get_handle(dev_addr);
    if (!dev_hdl) {
        ESP_LOGE(TAG, "claim_interfaces: no handle for addr %d", dev_addr);
        return ESP_ERR_NOT_FOUND;
    }

    const usb_config_desc_t *config_desc = NULL;
    esp_err_t err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (err != ESP_OK || !config_desc) {
        ESP_LOGE(TAG, "claim_interfaces: failed to get config desc: %s", esp_err_to_name(err));
        return err;
    }

    int offset = 0;
    const usb_standard_desc_t *cur = (const usb_standard_desc_t *)config_desc;
    while ((cur = usb_parse_next_descriptor_of_type(
                cur, config_desc->wTotalLength,
                USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
        if (intf->bAlternateSetting != 0) {
            continue;
        }
        err = usb_host_interface_claim(s_client_hdl, dev_hdl,
                                       intf->bInterfaceNumber, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to claim interface %d: %s",
                     intf->bInterfaceNumber, esp_err_to_name(err));
            /* Continue claiming other interfaces */
        } else {
            ESP_LOGI(TAG, "Claimed interface %d on addr %d",
                     intf->bInterfaceNumber, dev_addr);
        }
    }

    return ESP_OK;
}

esp_err_t usb_host_mgr_release_interfaces(uint8_t dev_addr)
{
    usb_device_handle_t dev_hdl = usb_host_mgr_get_handle(dev_addr);
    if (!dev_hdl) {
        ESP_LOGW(TAG, "release_interfaces: no handle for addr %d", dev_addr);
        return ESP_ERR_NOT_FOUND;
    }

    const usb_config_desc_t *config_desc = NULL;
    esp_err_t err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (err != ESP_OK || !config_desc) {
        ESP_LOGW(TAG, "release_interfaces: failed to get config desc: %s", esp_err_to_name(err));
        return err;
    }

    int offset = 0;
    const usb_standard_desc_t *cur = (const usb_standard_desc_t *)config_desc;
    while ((cur = usb_parse_next_descriptor_of_type(
                cur, config_desc->wTotalLength,
                USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
        if (intf->bAlternateSetting != 0) {
            continue;
        }
        err = usb_host_interface_release(s_client_hdl, dev_hdl,
                                         intf->bInterfaceNumber);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to release interface %d: %s",
                     intf->bInterfaceNumber, esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "Released interface %d on addr %d",
                     intf->bInterfaceNumber, dev_addr);
        }
    }

    return ESP_OK;
}

void usb_host_mgr_notify_removal(uint8_t dev_addr)
{
    s_removed_addr = dev_addr;
    ESP_LOGI(TAG, "Device removal notified: addr=%d", dev_addr);
}

uint8_t usb_host_mgr_check_removal(void)
{
    uint8_t addr = s_removed_addr;
    if (addr != 0) {
        s_removed_addr = 0;
    }
    return addr;
}

esp_err_t usb_host_mgr_reset_device(uint8_t dev_addr)
{
    ESP_LOGW(TAG, "Device reset not supported in current ESP-IDF version (addr=%d)", dev_addr);
    return ESP_ERR_NOT_SUPPORTED;
}
