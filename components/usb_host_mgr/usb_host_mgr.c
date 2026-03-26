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

#include <string.h>

static const char *TAG = "usb_host";

/* ---- Internal device tracking ---- */
#define MAX_TRACKED_DEVICES  DEVICE_MANAGER_MAX_DEVICES

typedef struct {
    bool in_use;
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
            s_tracked[i].in_use   = true;
            s_tracked[i].dev_addr = dev_addr;
            s_tracked[i].dev_hdl  = dev_hdl;
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

    /* 7. Add to device_manager */
    int dm_idx = -1;
    err = device_manager_add(&dm_info, &dm_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device to device_manager: %s", esp_err_to_name(err));
        /* Still keep it tracked so we can close it on disconnect */
    } else {
        ESP_LOGI(TAG, "Device added to device_manager at index %d, busid=%s", dm_idx, dm_info.path);
    }

    /* 8. Log interface descriptors from config descriptor */
    if (config_desc) {
        int offset = 0;
        const usb_standard_desc_t *cur = (const usb_standard_desc_t *)config_desc;
        while ((cur = usb_parse_next_descriptor_of_type(
                    cur, config_desc->wTotalLength,
                    USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)cur;
            ESP_LOGI(TAG, "  Interface %d: class=%d subclass=%d protocol=%d endpoints=%d",
                     intf->bInterfaceNumber, intf->bInterfaceClass,
                     intf->bInterfaceSubClass, intf->bInterfaceProtocol,
                     intf->bNumEndpoints);
        }
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
