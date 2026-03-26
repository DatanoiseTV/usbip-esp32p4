/*
 * Device Manager Component - Full Implementation
 * USB device registry with add/remove/lookup/import/release
 */

#include "device_manager.h"
#include "event_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "dev_mgr";

static struct {
    usb_device_info_t devices[CONFIG_USBIP_MAX_DEVICES];
    SemaphoreHandle_t mutex;
    bool initialized;
} s_devmgr = {0};

static inline bool take_lock(void)
{
    return xSemaphoreTake(s_devmgr.mutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

static inline void give_lock(void)
{
    xSemaphoreGive(s_devmgr.mutex);
}

esp_err_t device_manager_init(void)
{
    if (s_devmgr.initialized) {
        return ESP_OK;
    }

    memset(s_devmgr.devices, 0, sizeof(s_devmgr.devices));

    s_devmgr.mutex = xSemaphoreCreateMutex();
    if (s_devmgr.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_devmgr.initialized = true;
    ESP_LOGI(TAG, "Device manager initialized (max_devices=%d)", CONFIG_USBIP_MAX_DEVICES);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Device manager initialized");

    return ESP_OK;
}

esp_err_t device_manager_add(const usb_device_info_t *info, int *out_index)
{
    if (!s_devmgr.initialized || info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    int slot = -1;
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (!s_devmgr.devices[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        give_lock();
        ESP_LOGW(TAG, "Device registry full");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&s_devmgr.devices[slot], info, sizeof(usb_device_info_t));
    s_devmgr.devices[slot].in_use = true;
    s_devmgr.devices[slot].state = DEV_STATE_AVAILABLE;

    if (out_index) {
        *out_index = slot;
    }

    give_lock();

    ESP_LOGI(TAG, "Added device [%d]: VID=%04x PID=%04x path=%s",
             slot, info->vendor_id, info->product_id, info->path);
    event_log_add(EVENT_LOG_LEVEL_INFO, "USB device added: VID=%04x PID=%04x path=%s",
                  info->vendor_id, info->product_id, info->path);

    return ESP_OK;
}

esp_err_t device_manager_remove(int index)
{
    if (!s_devmgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || index >= CONFIG_USBIP_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_devmgr.devices[index].in_use) {
        give_lock();
        return ESP_ERR_NOT_FOUND;
    }

    usb_device_info_t *dev = &s_devmgr.devices[index];
    ESP_LOGI(TAG, "Removing device [%d]: VID=%04x PID=%04x path=%s",
             index, dev->vendor_id, dev->product_id, dev->path);
    event_log_add(EVENT_LOG_LEVEL_INFO, "USB device removed: path=%s", dev->path);

    memset(dev, 0, sizeof(usb_device_info_t));

    give_lock();
    return ESP_OK;
}

esp_err_t device_manager_lookup(const char *path, int *out_index)
{
    if (!s_devmgr.initialized || path == NULL || out_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (s_devmgr.devices[i].in_use && strcmp(s_devmgr.devices[i].path, path) == 0) {
            *out_index = i;
            give_lock();
            return ESP_OK;
        }
    }

    give_lock();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_manager_get(int index, usb_device_info_t *out_info)
{
    if (!s_devmgr.initialized || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (index < 0 || index >= CONFIG_USBIP_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_devmgr.devices[index].in_use) {
        give_lock();
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(out_info, &s_devmgr.devices[index], sizeof(usb_device_info_t));
    give_lock();
    return ESP_OK;
}

esp_err_t device_manager_import(int index, uint32_t client_ip)
{
    if (!s_devmgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || index >= CONFIG_USBIP_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    usb_device_info_t *dev = &s_devmgr.devices[index];
    if (!dev->in_use) {
        give_lock();
        return ESP_ERR_NOT_FOUND;
    }
    if (dev->state == DEV_STATE_EXPORTED) {
        give_lock();
        ESP_LOGW(TAG, "Device [%d] already exported", index);
        return ESP_ERR_INVALID_STATE;
    }

    dev->state = DEV_STATE_EXPORTED;
    dev->client_ip = client_ip;

    give_lock();

    uint8_t *ip = (uint8_t *)&client_ip;
    ESP_LOGI(TAG, "Device [%d] exported to %d.%d.%d.%d",
             index, ip[0], ip[1], ip[2], ip[3]);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s exported to %d.%d.%d.%d",
                  dev->path, ip[0], ip[1], ip[2], ip[3]);

    return ESP_OK;
}

esp_err_t device_manager_release(int index)
{
    if (!s_devmgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || index >= CONFIG_USBIP_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    usb_device_info_t *dev = &s_devmgr.devices[index];
    if (!dev->in_use) {
        give_lock();
        return ESP_ERR_NOT_FOUND;
    }

    dev->state = DEV_STATE_AVAILABLE;
    dev->client_ip = 0;

    give_lock();

    ESP_LOGI(TAG, "Device [%d] released", index);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s released", dev->path);

    return ESP_OK;
}

int device_manager_get_count(void)
{
    if (!s_devmgr.initialized) {
        return 0;
    }
    if (!take_lock()) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (s_devmgr.devices[i].in_use) {
            count++;
        }
    }

    give_lock();
    return count;
}

void device_manager_foreach(bool (*callback)(int index, const usb_device_info_t *info, void *user_data), void *user_data)
{
    if (!s_devmgr.initialized || callback == NULL) {
        return;
    }
    if (!take_lock()) {
        return;
    }

    for (int i = 0; i < CONFIG_USBIP_MAX_DEVICES; i++) {
        if (s_devmgr.devices[i].in_use) {
            if (!callback(i, &s_devmgr.devices[i], user_data)) {
                break;
            }
        }
    }

    give_lock();
}
