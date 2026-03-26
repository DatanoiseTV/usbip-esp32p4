/*
 * Web UI - Stub
 */

#include "webui.h"
#include "esp_log.h"

static const char *TAG = "webui";

esp_err_t webui_init(void)
{
    ESP_LOGI(TAG, "Web UI initialized (stub)");
    return ESP_OK;
}

esp_err_t webui_stop(void)
{
    ESP_LOGI(TAG, "Web UI stopped (stub)");
    return ESP_OK;
}
