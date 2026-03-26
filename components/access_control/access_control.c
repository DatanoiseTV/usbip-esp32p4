/*
 * Access Control - Stub
 */

#include "access_control.h"
#include "esp_log.h"

static const char *TAG = "acl";

esp_err_t access_control_init(void)
{
    ESP_LOGI(TAG, "Access control initialized (stub - all clients allowed)");
    return ESP_OK;
}

bool access_control_check(uint32_t client_ip)
{
    (void)client_ip;
    return true; /* Allow all in stub */
}
