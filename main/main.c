/*
 * USB/IP Server for ESP32-P4-Nano
 * Main application entry point
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "event_log.h"
#include "access_control.h"
#include "device_manager.h"
#include "network_mgr.h"
#include "usb_host_mgr.h"
#include "transfer_engine.h"
#include "usbip_server.h"
#include "webui.h"
#if CONFIG_CAPTURE_ENABLED
#include "capture.h"
#endif

static const char *TAG = "main";

/*
 * Custom log handler: intercepts ESP-IDF log output to detect USB hub TT errors
 * and enum failures, forwarding them to our event log for WebUI display.
 */
static vprintf_like_t s_orig_vprintf = NULL;

static int custom_vprintf(const char *fmt, va_list args)
{
    /* Call original handler first */
    int ret = s_orig_vprintf(fmt, args);

    /* Check for known USB hub/enum error patterns in the format string */
    if (fmt) {
        if (strstr(fmt, "transaction translator") || strstr(fmt, "TT") ) {
            event_log_add(EVENT_LOG_LEVEL_WARN,
                "USB hub: FS/LS device rejected (no TT support). Connect directly to root port.");
        } else if (strstr(fmt, "CHECK_SHORT_DEV_DESC FAILED") ||
                   strstr(fmt, "CHECK_FULL_CONFIG_DESC FAILED")) {
            event_log_add(EVENT_LOG_LEVEL_WARN,
                "USB device enumeration failed. Try reconnecting the device.");
        }
    }

    return ret;
}

void app_main(void)
{
    esp_err_t ret;

    /* Log CPU frequency */
    uint32_t cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    ESP_LOGI(TAG, "CPU frequency: %lu MHz", (unsigned long)cpu_freq_mhz);

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

    /* Install custom log handler to catch USB hub TT errors for WebUI */
    s_orig_vprintf = esp_log_set_vprintf(custom_vprintf);
    ESP_LOGI(TAG, "Custom log handler installed");

    ESP_ERROR_CHECK(access_control_init());
    ESP_LOGI(TAG, "Access control initialized");

    ESP_ERROR_CHECK(device_manager_init());
    ESP_LOGI(TAG, "Device manager initialized");

    ESP_ERROR_CHECK(network_mgr_init());
    ESP_LOGI(TAG, "Network manager initialized");

    ESP_ERROR_CHECK(usb_host_mgr_init());
    ESP_LOGI(TAG, "USB host manager initialized");

    ESP_ERROR_CHECK(transfer_engine_init());
    ESP_LOGI(TAG, "Transfer engine initialized");

#if CONFIG_CAPTURE_ENABLED
    /* capture_init is non-fatal if SD card is absent */
    esp_err_t cap_ret = capture_init();
    if (cap_ret == ESP_OK) {
        ESP_LOGI(TAG, "Capture subsystem initialized");
    } else {
        ESP_LOGW(TAG, "Capture init: %s (non-fatal)", esp_err_to_name(cap_ret));
    }
#endif

    ESP_ERROR_CHECK(usbip_server_init());
    ESP_LOGI(TAG, "USB/IP server initialized");

    ESP_ERROR_CHECK(webui_init());
    ESP_LOGI(TAG, "Web UI initialized");

    /* Log free heap after all init */
    size_t free_heap = esp_get_free_heap_size();
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Free heap: %u bytes (internal: %u bytes)", (unsigned)free_heap, (unsigned)free_internal);

    /* Initial event log entry */
    event_log_add(EVENT_LOG_LEVEL_INFO, "System started: CPU %lu MHz, PSRAM %.1f MB, free heap %u bytes",
                  (unsigned long)cpu_freq_mhz, (float)psram_size / (1024.0f * 1024.0f), (unsigned)free_heap);

    ESP_LOGI(TAG, "USB/IP server ready. Connect via: usbip list -r <ip>");
}
