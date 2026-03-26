/*
 * Transfer Engine - Stub
 */

#include "transfer_engine.h"
#include "esp_log.h"

static const char *TAG = "xfer_eng";

esp_err_t transfer_engine_init(void)
{
    ESP_LOGI(TAG, "Transfer engine initialized (stub)");
    return ESP_OK;
}

esp_err_t transfer_engine_stop(void)
{
    ESP_LOGI(TAG, "Transfer engine stopped (stub)");
    return ESP_OK;
}
