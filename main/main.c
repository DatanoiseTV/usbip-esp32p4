/*
 * USB/IP Server for ESP32-P4-Nano
 * Main application entry point
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_psram.h"

#include "event_log.h"
#include "access_control.h"
#include "device_manager.h"
#include "network_mgr.h"
#include "usb_host_mgr.h"
#include "usbip_server.h"
#include "webui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret;

    /* Initialize NVS */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Log PSRAM info */
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM size: %u bytes (%.1f MB)", (unsigned)psram_size,
             (float)psram_size / (1024.0f * 1024.0f));

    /* Initialize subsystems in dependency order */
    ESP_LOGI(TAG, "Initializing subsystems...");

    ESP_ERROR_CHECK(event_log_init());
    ESP_LOGI(TAG, "Event log initialized");

    ESP_ERROR_CHECK(access_control_init());
    ESP_LOGI(TAG, "Access control initialized");

    ESP_ERROR_CHECK(device_manager_init());
    ESP_LOGI(TAG, "Device manager initialized");

    ESP_ERROR_CHECK(network_mgr_init());
    ESP_LOGI(TAG, "Network manager initialized");

    ESP_ERROR_CHECK(usb_host_mgr_init());
    ESP_LOGI(TAG, "USB host manager initialized");

    ESP_ERROR_CHECK(usbip_server_init());
    ESP_LOGI(TAG, "USB/IP server initialized");

    ESP_ERROR_CHECK(webui_init());
    ESP_LOGI(TAG, "Web UI initialized");

    ESP_LOGI(TAG, "USB/IP server ready");
}
