/*
 * Web UI - HTTP server with embedded frontend and WebSocket support
 */

#include "webui.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "device_manager.h"
#include "event_log.h"
#include "access_control.h"
#include "network_mgr.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/base64.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

static const char *TAG = "webui";

/* ---- Authentication ---- */

#define AUTH_NVS_NAMESPACE "auth"
#define AUTH_MAX_USERNAME 32

static bool s_auth_enabled = false;
static char s_auth_username[AUTH_MAX_USERNAME] = "";
static uint8_t s_auth_password_hash[32] = {0};

static void auth_load_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(AUTH_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t en = 0;
    nvs_get_u8(nvs, "enabled", &en);
    s_auth_enabled = (en != 0);
    size_t len = sizeof(s_auth_username);
    nvs_get_str(nvs, "username", s_auth_username, &len);
    len = sizeof(s_auth_password_hash);
    nvs_get_blob(nvs, "pw_hash", s_auth_password_hash, &len);
    nvs_close(nvs);
}

void webui_auth_save(bool enabled, const char *username, const char *password)
{
    s_auth_enabled = enabled;
    if (username) strncpy(s_auth_username, username, AUTH_MAX_USERNAME - 1);
    if (password) {
        mbedtls_sha256((const uint8_t *)password, strlen(password), s_auth_password_hash, 0);
    }
    nvs_handle_t nvs;
    if (nvs_open(AUTH_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "enabled", enabled ? 1 : 0);
    nvs_set_str(nvs, "username", s_auth_username);
    nvs_set_blob(nvs, "pw_hash", s_auth_password_hash, 32);
    nvs_commit(nvs);
    nvs_close(nvs);
}

bool webui_auth_enabled(void)
{
    return s_auth_enabled;
}

const char *webui_auth_username(void)
{
    return s_auth_username;
}

static bool auth_check_request(httpd_req_t *req)
{
    if (!s_auth_enabled) return true;

    char auth_buf[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf)) != ESP_OK) {
        return false;
    }
    /* Expect "Basic base64encoded" */
    if (strncmp(auth_buf, "Basic ", 6) != 0) return false;

    /* Decode base64 */
    unsigned char decoded[128];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *)(auth_buf + 6), strlen(auth_buf + 6)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    /* Split on ':' */
    char *colon = strchr((char *)decoded, ':');
    if (!colon) return false;
    *colon = '\0';
    const char *user = (char *)decoded;
    const char *pass = colon + 1;

    /* Check username */
    if (strcmp(user, s_auth_username) != 0) return false;

    /* Check password hash */
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t *)pass, strlen(pass), hash, 0);
    return memcmp(hash, s_auth_password_hash, 32) == 0;
}

static esp_err_t auth_reject(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"USB/IP Server\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Public auth wrappers for use by webui_api.c */
bool webui_check_auth(httpd_req_t *req)
{
    return auth_check_request(req);
}

esp_err_t webui_reject_auth(httpd_req_t *req)
{
    return auth_reject(req);
}

/* External symbols for embedded frontend files */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t settings_html_end[]   asm("_binary_settings_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
extern const uint8_t htmx_min_js_start[] asm("_binary_htmx_min_js_start");
extern const uint8_t htmx_min_js_end[]   asm("_binary_htmx_min_js_end");

/* Declarations from ws_handler.c */
extern void ws_handler_init(httpd_handle_t server);
extern esp_err_t ws_handler(httpd_req_t *req);
extern void ws_broadcast_stats(void);
extern void ws_on_close(int fd);

static httpd_handle_t s_server = NULL;
static esp_timer_handle_t s_stats_timer = NULL;

/* ---- Embedded file serving handlers ---- */

static esp_err_t handle_index(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start,
                    index_html_end - index_html_start);
    return ESP_OK;
}

static esp_err_t handle_settings(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)settings_html_start,
                    settings_html_end - settings_html_start);
    return ESP_OK;
}

static esp_err_t handle_style_css(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)style_css_start,
                    style_css_end - style_css_start);
    return ESP_OK;
}

static esp_err_t handle_htmx_js(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)htmx_min_js_start,
                    htmx_min_js_end - htmx_min_js_start);
    return ESP_OK;
}

/* ---- JSON API handlers ---- */

static esp_err_t handle_api_devices(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "application/json");

    /* Build JSON array of devices */
    char json_buf[2048];
    int pos = 0;
    int buflen = sizeof(json_buf);

    pos += snprintf(json_buf + pos, buflen - pos, "[");
    int dev_count = device_manager_get_count();
    int printed = 0;

    for (int i = 0; i < DEVICE_MANAGER_MAX_DEVICES && printed < dev_count; i++) {
        dm_device_info_t info;
        if (device_manager_get(i, &info) != ESP_OK || !info.in_use) continue;

        if (printed > 0) pos += snprintf(json_buf + pos, buflen - pos, ",");

        pos += snprintf(json_buf + pos, buflen - pos,
            "{\"index\":%d,\"path\":\"%s\",\"vid\":%u,\"pid\":%u,"
            "\"speed\":%d,\"state\":%d,\"client_ip\":%lu,"
            "\"class\":%u,\"subclass\":%u,\"protocol\":%u}",
            i, info.path,
            (unsigned)info.vendor_id, (unsigned)info.product_id,
            (int)info.speed, (int)info.state,
            (unsigned long)info.client_ip,
            (unsigned)info.dev_class, (unsigned)info.dev_subclass,
            (unsigned)info.dev_protocol);
        printed++;
    }

    pos += snprintf(json_buf + pos, buflen - pos, "]");
    httpd_resp_send(req, json_buf, pos);
    return ESP_OK;
}

static esp_err_t handle_api_stats(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "application/json");

    char json_buf[256];
    uint32_t free_heap = esp_get_free_heap_size();
    int64_t uptime_us = esp_timer_get_time();
    int uptime_sec = (int)(uptime_us / 1000000);
    int dev_count = device_manager_get_count();

    char ip_str[20] = "0.0.0.0";
    network_mgr_get_ip_str(ip_str, sizeof(ip_str));

    int len = snprintf(json_buf, sizeof(json_buf),
        "{\"free_heap\":%lu,\"uptime_sec\":%d,\"device_count\":%d,"
        "\"ip\":\"%s\",\"idf_version\":\"%s\"}",
        (unsigned long)free_heap, uptime_sec, dev_count,
        ip_str, esp_get_idf_version());

    httpd_resp_send(req, json_buf, len);
    return ESP_OK;
}

static esp_err_t handle_api_logs(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "application/json");

    char json_buf[4096];
    int pos = 0;
    int buflen = sizeof(json_buf);

    event_log_entry_t entries[50];
    size_t count = 0;
    event_log_get_recent(entries, 50, &count);

    pos += snprintf(json_buf + pos, buflen - pos, "[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(json_buf + pos, buflen - pos, ",");

        /* Escape message for JSON */
        char escaped[EVENT_LOG_MSG_MAX_LEN * 2];
        int ep = 0;
        for (int j = 0; entries[i].message[j] && ep < (int)sizeof(escaped) - 2; j++) {
            char c = entries[i].message[j];
            if (c == '"' || c == '\\') escaped[ep++] = '\\';
            if (c == '\n') { escaped[ep++] = '\\'; escaped[ep++] = 'n'; continue; }
            escaped[ep++] = c;
        }
        escaped[ep] = '\0';

        pos += snprintf(json_buf + pos, buflen - pos,
            "{\"ts\":%lld,\"level\":%d,\"msg\":\"%s\"}",
            entries[i].timestamp_us,
            (int)entries[i].level,
            escaped);
    }
    pos += snprintf(json_buf + pos, buflen - pos, "]");

    httpd_resp_send(req, json_buf, pos);
    return ESP_OK;
}

static esp_err_t handle_api_restart(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"restarting\"}", -1);

    /* Delay slightly to allow response to be sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* ---- WebSocket handler wrapper ---- */

static esp_err_t handle_ws(httpd_req_t *req)
{
    if (!auth_check_request(req)) return auth_reject(req);
    return ws_handler(req);
}

/* ---- Socket close hook ---- */

static void on_sock_close(httpd_handle_t hd, int sockfd)
{
    ws_on_close(sockfd);
    close(sockfd);
}

/* ---- Stats broadcast timer callback ---- */

static void stats_timer_cb(void *arg)
{
    ws_broadcast_stats();
}

/* ---- Public API ---- */

esp_err_t webui_init(void)
{
    auth_load_from_nvs();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.core_id = 1;
    config.task_priority = 5;
    config.stack_size = 8192;
    config.max_uri_handlers = 26;
    config.max_open_sockets = 4;
    config.close_fn = on_sock_close;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize WebSocket handler */
    ws_handler_init(s_server);

    /* Register URI handlers */

    /* Static file routes */
    const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET,
        .handler = handle_index, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_index);

    const httpd_uri_t uri_settings = {
        .uri = "/settings", .method = HTTP_GET,
        .handler = handle_settings, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_settings);

    const httpd_uri_t uri_css = {
        .uri = "/style.css", .method = HTTP_GET,
        .handler = handle_style_css, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_css);

    const httpd_uri_t uri_htmx = {
        .uri = "/htmx.min.js", .method = HTTP_GET,
        .handler = handle_htmx_js, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_htmx);

    /* API routes */
    const httpd_uri_t uri_api_devices = {
        .uri = "/api/devices", .method = HTTP_GET,
        .handler = handle_api_devices, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_api_devices);

    const httpd_uri_t uri_api_stats = {
        .uri = "/api/stats", .method = HTTP_GET,
        .handler = handle_api_stats, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_api_stats);

    const httpd_uri_t uri_api_logs = {
        .uri = "/api/logs", .method = HTTP_GET,
        .handler = handle_api_logs, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_api_logs);

    const httpd_uri_t uri_api_restart = {
        .uri = "/api/restart", .method = HTTP_POST,
        .handler = handle_api_restart, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_api_restart);

    /* WebSocket route */
    const httpd_uri_t uri_ws = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = handle_ws, .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &uri_ws);

    /* Register REST API endpoints */
    webui_api_register(s_server);

    /* Create stats broadcast timer (500ms interval) */
    const esp_timer_create_args_t timer_args = {
        .callback = stats_timer_cb,
        .name = "ws_stats",
    };
    ret = esp_timer_create(&timer_args, &s_stats_timer);
    if (ret == ESP_OK) {
        esp_timer_start_periodic(s_stats_timer, 500 * 1000); /* 500ms in us */
    }

    ESP_LOGI(TAG, "Web UI initialized on port 80");
    return ESP_OK;
}

esp_err_t webui_stop(void)
{
    if (s_stats_timer) {
        esp_timer_stop(s_stats_timer);
        esp_timer_delete(s_stats_timer);
        s_stats_timer = NULL;
    }

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    ESP_LOGI(TAG, "Web UI stopped");
    return ESP_OK;
}

void webui_notify_device_change(void)
{
    /* Trigger an immediate stats broadcast when devices change */
    ws_broadcast_stats();
}
