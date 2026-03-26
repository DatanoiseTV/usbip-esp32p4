/*
 * WebSocket Handler
 * Manages WS client connections and broadcasts real-time stats
 */

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "device_manager.h"
#include "event_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ws_handler";

#define WS_MAX_CLIENTS 2

/* Internal state */
static int ws_fds[WS_MAX_CLIENTS];
static httpd_handle_t ws_server_handle;
static portMUX_TYPE ws_fds_lock = portMUX_INITIALIZER_UNLOCKED;

void ws_handler_init(httpd_handle_t server)
{
    ws_server_handle = server;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_fds[i] = -1;
    }
    ESP_LOGI(TAG, "WebSocket handler initialized (max_clients=%d)", WS_MAX_CLIENTS);
}

static void ws_register_fd(int fd)
{
    portENTER_CRITICAL(&ws_fds_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_fds[i] == fd) {
            portEXIT_CRITICAL(&ws_fds_lock);
            return; /* already registered */
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_fds[i] < 0) {
            ws_fds[i] = fd;
            portEXIT_CRITICAL(&ws_fds_lock);
            ESP_LOGI(TAG, "WS client registered fd=%d slot=%d", fd, i);
            return;
        }
    }
    portEXIT_CRITICAL(&ws_fds_lock);
    ESP_LOGW(TAG, "WS client slots full, rejecting fd=%d", fd);
}

static void ws_unregister_fd(int fd)
{
    portENTER_CRITICAL(&ws_fds_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_fds[i] == fd) {
            ws_fds[i] = -1;
            portEXIT_CRITICAL(&ws_fds_lock);
            ESP_LOGI(TAG, "WS client unregistered fd=%d slot=%d", fd, i);
            return;
        }
    }
    portEXIT_CRITICAL(&ws_fds_lock);
}

/* Build device JSON fragment into buf. Returns chars written. */
static int build_devices_json(char *buf, int buflen)
{
    int pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "[");

    int dev_count = device_manager_get_count();
    int printed = 0;

    for (int i = 0; i < DEVICE_MANAGER_MAX_DEVICES && printed < dev_count; i++) {
        dm_device_info_t info;
        if (device_manager_get(i, &info) != ESP_OK || !info.in_use) {
            continue;
        }
        if (printed > 0) {
            pos += snprintf(buf + pos, buflen - pos, ",");
        }
        pos += snprintf(buf + pos, buflen - pos,
            "{\"path\":\"%s\",\"vid\":%u,\"pid\":%u,\"speed\":%d,"
            "\"state\":%d,\"client_ip\":%lu}",
            info.path,
            (unsigned)info.vendor_id,
            (unsigned)info.product_id,
            (int)info.speed,
            (int)info.state,
            (unsigned long)info.client_ip);
        printed++;
    }

    pos += snprintf(buf + pos, buflen - pos, "]");
    return pos;
}

/* Build recent log entries JSON fragment */
static int build_logs_json(char *buf, int buflen)
{
    int pos = 0;
    event_log_entry_t entries[20];
    size_t count = 0;

    event_log_get_recent(entries, 20, &count);

    pos += snprintf(buf + pos, buflen - pos, "[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) pos += snprintf(buf + pos, buflen - pos, ",");
        /* Escape any quotes in message */
        char escaped[EVENT_LOG_MSG_MAX_LEN * 2];
        int ep = 0;
        for (int j = 0; entries[i].message[j] && ep < (int)sizeof(escaped) - 2; j++) {
            char c = entries[i].message[j];
            if (c == '"' || c == '\\') {
                escaped[ep++] = '\\';
            }
            if (c == '\n') {
                escaped[ep++] = '\\';
                escaped[ep++] = 'n';
                continue;
            }
            escaped[ep++] = c;
        }
        escaped[ep] = '\0';

        pos += snprintf(buf + pos, buflen - pos,
            "{\"ts\":%lld,\"level\":%d,\"msg\":\"%s\"}",
            entries[i].timestamp_us,
            (int)entries[i].level,
            escaped);
    }
    pos += snprintf(buf + pos, buflen - pos, "]");
    return pos;
}

void ws_broadcast_stats(void)
{
    /* Snapshot fds under lock */
    int fds[WS_MAX_CLIENTS];
    portENTER_CRITICAL(&ws_fds_lock);
    bool has_clients = false;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        fds[i] = ws_fds[i];
        if (fds[i] >= 0) has_clients = true;
    }
    portEXIT_CRITICAL(&ws_fds_lock);
    if (!has_clients) return;

    /* Build the stats JSON message on the stack */
    char *json_buf = malloc(4096);
    if (!json_buf) {
        ESP_LOGW(TAG, "ws_broadcast_stats: OOM");
        return;
    }
    int pos = 0;
    int buflen = 4096;

    uint32_t free_heap = esp_get_free_heap_size();
    int64_t uptime_us = esp_timer_get_time();
    int uptime_sec = (int)(uptime_us / 1000000);

    pos += snprintf(json_buf + pos, buflen - pos,
        "{\"type\":\"stats\",\"free_heap\":%lu,\"uptime_sec\":%d,"
        "\"bw_in\":0,\"bw_out\":0,\"devices\":",
        (unsigned long)free_heap, uptime_sec);

    pos += build_devices_json(json_buf + pos, buflen - pos);

    pos += snprintf(json_buf + pos, buflen - pos, ",\"logs\":");
    pos += build_logs_json(json_buf + pos, buflen - pos);

    pos += snprintf(json_buf + pos, buflen - pos, "}");

    /* Send to all connected clients */
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_buf,
        .len = pos,
        .final = true,
    };

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (fds[i] < 0) continue;
        esp_err_t ret = httpd_ws_send_frame_async(ws_server_handle, fds[i], &frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WS send failed fd=%d: %s, unregistering", fds[i], esp_err_to_name(ret));
            ws_unregister_fd(fds[i]);
        }
    }

    free(json_buf);
}

/* Handle incoming WS messages */
static void handle_ws_message(const char *data, int len)
{
    /* Simple JSON parse: look for "action" and "busid" */
    const char *act = strstr(data, "\"action\"");
    if (!act) return;

    if (strstr(act, "\"toggle_export\"")) {
        const char *busid_key = strstr(data, "\"busid\"");
        if (!busid_key) return;
        /* Find the value */
        const char *colon = strchr(busid_key + 7, ':');
        if (!colon) return;
        const char *quote1 = strchr(colon, '"');
        if (!quote1) return;
        quote1++;
        const char *quote2 = strchr(quote1, '"');
        if (!quote2 || quote2 - quote1 >= 32) return;

        char busid[32];
        int blen = (int)(quote2 - quote1);
        memcpy(busid, quote1, blen);
        busid[blen] = '\0';

        int idx;
        if (device_manager_lookup(busid, &idx) == ESP_OK) {
            dm_device_info_t info;
            if (device_manager_get(idx, &info) == ESP_OK) {
                if (info.state == DEV_STATE_EXPORTED) {
                    device_manager_release(idx);
                    ESP_LOGI(TAG, "Released device %s via WS", busid);
                }
                /* Note: actual export requires a USBIP client connection,
                   so we can only release from the web UI */
            }
        }
    }
}

/* WebSocket handler called by esp_http_server */
esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* This is the initial WS handshake GET - register client for broadcasts */
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS handshake from fd=%d", fd);
        ws_register_fd(fd);
        return ESP_OK;
    }

    /* Receive the WS frame */
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame length failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (frame.len == 0) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;

    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame data failed: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_register_fd(fd);

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        handle_ws_message((char *)buf, frame.len);
    }

    free(buf);
    return ESP_OK;
}

void ws_on_close(int fd)
{
    ws_unregister_fd(fd);
}
