/*
 * Network Manager - Stub
 */

#include "network_mgr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "net_mgr";

esp_err_t network_mgr_init(void)
{
    ESP_LOGI(TAG, "Network manager initialized (stub)");
    return ESP_OK;
}

esp_err_t network_mgr_get_ip_str(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(buf, "0.0.0.0", buf_len);
    return ESP_OK;
}
