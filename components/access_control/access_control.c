/*
 * Access Control - Full implementation with NVS persistence
 */

#include "access_control.h"
#include "event_log.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <lwip/inet.h>

static const char *TAG = "acl";
static const char *NVS_NAMESPACE = "acl";
static const char *NVS_KEY_CLOSED = "closed";
static const char *NVS_KEY_ALLOWLIST = "allowlist";

static bool s_closed_mode = false;
static uint32_t s_allowlist[ACCESS_CONTROL_MAX_IPS];
static int s_allowlist_count = 0;

/* Format an IP (network byte order) into a dotted string */
static void ip_to_str(uint32_t ip, char *buf, size_t len)
{
    uint8_t *b = (uint8_t *)&ip;
    snprintf(buf, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static esp_err_t save_mode_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing mode: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t val = s_closed_mode ? 1 : 0;
    err = nvs_set_u8(nvs, NVS_KEY_CLOSED, val);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save mode to NVS: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t save_allowlist_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing allowlist: %s", esp_err_to_name(err));
        return err;
    }

    if (s_allowlist_count > 0) {
        err = nvs_set_blob(nvs, NVS_KEY_ALLOWLIST, s_allowlist,
                           s_allowlist_count * sizeof(uint32_t));
    } else {
        /* Store empty blob when list is empty */
        err = nvs_set_blob(nvs, NVS_KEY_ALLOWLIST, s_allowlist, 0);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save allowlist to NVS: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t access_control_init(void)
{
    s_closed_mode = false;
    s_allowlist_count = 0;
    memset(s_allowlist, 0, sizeof(s_allowlist));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No ACL data in NVS, using defaults (open mode)");
        event_log_add(EVENT_LOG_LEVEL_INFO, "Access control initialized (open mode, no saved config)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(err));
        event_log_add(EVENT_LOG_LEVEL_WARN, "Access control init: NVS read failed, using defaults");
        return ESP_OK; /* Non-fatal: use defaults */
    }

    /* Load mode */
    uint8_t closed_val = 0;
    err = nvs_get_u8(nvs, NVS_KEY_CLOSED, &closed_val);
    if (err == ESP_OK) {
        s_closed_mode = (closed_val != 0);
    }

    /* Load allowlist */
    size_t blob_len = 0;
    err = nvs_get_blob(nvs, NVS_KEY_ALLOWLIST, NULL, &blob_len);
    if (err == ESP_OK && blob_len > 0) {
        if (blob_len > sizeof(s_allowlist)) {
            blob_len = sizeof(s_allowlist);
        }
        err = nvs_get_blob(nvs, NVS_KEY_ALLOWLIST, s_allowlist, &blob_len);
        if (err == ESP_OK) {
            s_allowlist_count = blob_len / sizeof(uint32_t);
        }
    }

    nvs_close(nvs);

    ESP_LOGI(TAG, "Access control initialized: mode=%s, %d IPs in allowlist",
             s_closed_mode ? "closed" : "open", s_allowlist_count);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Access control initialized: %s mode, %d allowed IPs",
                  s_closed_mode ? "closed" : "open", s_allowlist_count);

    return ESP_OK;
}

bool access_control_check(uint32_t client_ip)
{
    if (!s_closed_mode) {
        return true;
    }

    for (int i = 0; i < s_allowlist_count; i++) {
        if (s_allowlist[i] == client_ip) {
            return true;
        }
    }

    char ip_str[16];
    ip_to_str(client_ip, ip_str, sizeof(ip_str));
    ESP_LOGW(TAG, "Denied connection from %s (not in allowlist)", ip_str);
    event_log_add(EVENT_LOG_LEVEL_WARN, "Access denied for %s", ip_str);
    return false;
}

bool access_control_is_closed_mode(void)
{
    return s_closed_mode;
}

void access_control_set_mode(bool closed)
{
    if (s_closed_mode == closed) {
        return; /* No change */
    }

    s_closed_mode = closed;
    save_mode_to_nvs();

    const char *mode_str = closed ? "closed" : "open";
    ESP_LOGI(TAG, "Access control mode set to %s", mode_str);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Access control mode changed to %s", mode_str);
}

esp_err_t access_control_add_ip(uint32_t ip)
{
    char ip_str[16];
    ip_to_str(ip, ip_str, sizeof(ip_str));

    /* Check for duplicate */
    for (int i = 0; i < s_allowlist_count; i++) {
        if (s_allowlist[i] == ip) {
            ESP_LOGW(TAG, "IP %s already in allowlist", ip_str);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_allowlist_count >= ACCESS_CONTROL_MAX_IPS) {
        ESP_LOGE(TAG, "Allowlist full, cannot add %s", ip_str);
        return ESP_ERR_NO_MEM;
    }

    s_allowlist[s_allowlist_count++] = ip;
    save_allowlist_to_nvs();

    ESP_LOGI(TAG, "Added %s to allowlist (%d total)", ip_str, s_allowlist_count);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Added %s to access allowlist", ip_str);

    return ESP_OK;
}

esp_err_t access_control_remove_ip(uint32_t ip)
{
    char ip_str[16];
    ip_to_str(ip, ip_str, sizeof(ip_str));

    for (int i = 0; i < s_allowlist_count; i++) {
        if (s_allowlist[i] == ip) {
            /* Shift remaining entries down */
            for (int j = i; j < s_allowlist_count - 1; j++) {
                s_allowlist[j] = s_allowlist[j + 1];
            }
            s_allowlist_count--;
            save_allowlist_to_nvs();

            ESP_LOGI(TAG, "Removed %s from allowlist (%d remaining)", ip_str, s_allowlist_count);
            event_log_add(EVENT_LOG_LEVEL_INFO, "Removed %s from access allowlist", ip_str);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "IP %s not found in allowlist", ip_str);
    return ESP_ERR_NOT_FOUND;
}

int access_control_get_allowlist(uint32_t *out_ips, int max)
{
    if (out_ips == NULL || max <= 0) {
        return 0;
    }

    int count = (s_allowlist_count < max) ? s_allowlist_count : max;
    memcpy(out_ips, s_allowlist, count * sizeof(uint32_t));
    return count;
}
