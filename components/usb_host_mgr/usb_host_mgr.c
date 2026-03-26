/*
 * USB Host Manager - Stub
 */

#include "usb_host_mgr.h"
#include "esp_log.h"

static const char *TAG = "usb_host";

esp_err_t usb_host_mgr_init(void)
{
    ESP_LOGI(TAG, "USB host manager initialized (stub)");
    return ESP_OK;
}

esp_err_t usb_host_mgr_stop(void)
{
    ESP_LOGI(TAG, "USB host manager stopped (stub)");
    return ESP_OK;
}
