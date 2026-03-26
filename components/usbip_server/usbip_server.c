/*
 * USB/IP Server - Stub
 */

#include "usbip_server.h"
#include "esp_log.h"

static const char *TAG = "usbip_srv";

esp_err_t usbip_server_init(void)
{
    ESP_LOGI(TAG, "USB/IP server initialized (stub)");
    return ESP_OK;
}

esp_err_t usbip_server_stop(void)
{
    ESP_LOGI(TAG, "USB/IP server stopped (stub)");
    return ESP_OK;
}
