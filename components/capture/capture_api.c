/*
 * Packet Capture - HTTP API Endpoints
 */

#include "capture.h"
#include "webui.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "capture_api";

/* ---- Helpers ---- */

static esp_err_t send_json(httpd_req_t *req, const char *json, int len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t send_json_error(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, (status == 400) ? "400 Bad Request" :
                                (status == 404) ? "404 Not Found" :
                                "500 Internal Server Error");
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return send_json(req, buf, n);
}

/* ---- GET /api/capture/status ---- */

static esp_err_t handle_capture_status(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    capture_status_t st;
    capture_get_status(&st);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"card_present\":%s,\"capturing\":%s,"
        "\"filename\":\"%s\",\"file_size\":%lu,"
        "\"packet_count\":%lu,\"dropped_count\":%lu,"
        "\"ring_buf_used\":%lu,\"ring_buf_size\":%lu}",
        st.card_present ? "true" : "false",
        st.capturing ? "true" : "false",
        st.filename,
        (unsigned long)st.file_size,
        (unsigned long)st.packet_count,
        (unsigned long)st.dropped_count,
        (unsigned long)st.ring_buf_used,
        (unsigned long)st.ring_buf_size);

    return send_json(req, buf, n);
}

/* ---- POST /api/capture/start ---- */

static esp_err_t handle_capture_start(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    esp_err_t err = capture_start();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, 400, "Already capturing");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, 400, "No SD card present");
    }
    if (err != ESP_OK) {
        return send_json_error(req, 500, "Failed to start capture");
    }

    capture_status_t st;
    capture_get_status(&st);

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"filename\":\"%s\"}", st.filename);
    return send_json(req, buf, n);
}

/* ---- POST /api/capture/stop ---- */

static esp_err_t handle_capture_stop(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    capture_stop();
    return send_json(req, "{\"ok\":true}", -1);
}

/* ---- GET /api/capture/download ---- */

static esp_err_t handle_capture_download(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    const char *path = capture_get_filepath();
    if (!path || path[0] == '\0') {
        return send_json_error(req, 404, "No capture file");
    }

    /* If capturing, stop first to flush */
    capture_status_t st;
    capture_get_status(&st);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return send_json_error(req, 404, "Capture file not found");
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Set response headers */
    httpd_resp_set_type(req, "application/octet-stream");

    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", st.filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%ld", fsize);
    httpd_resp_set_hdr(req, "Content-Length", len_str);

    /* Stream file in 4 KB chunks */
    char chunk[4096];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            fclose(f);
            ESP_LOGE(TAG, "Download aborted by client");
            return ESP_FAIL;
        }
    }

    fclose(f);

    /* End chunked response */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- POST /api/capture/delete ---- */

static esp_err_t handle_capture_delete(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    esp_err_t err = capture_delete_file();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, 400, "Stop capture first");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_error(req, 404, "No capture file to delete");
    }
    if (err != ESP_OK) {
        return send_json_error(req, 500, "Delete failed");
    }

    return send_json(req, "{\"ok\":true}", -1);
}

/* ---- Route registration ---- */

void capture_api_register(httpd_handle_t server)
{
    const httpd_uri_t uri_status = {
        .uri = "/api/capture/status", .method = HTTP_GET,
        .handler = handle_capture_status, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_status);

    const httpd_uri_t uri_start = {
        .uri = "/api/capture/start", .method = HTTP_POST,
        .handler = handle_capture_start, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_start);

    const httpd_uri_t uri_stop = {
        .uri = "/api/capture/stop", .method = HTTP_POST,
        .handler = handle_capture_stop, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_stop);

    const httpd_uri_t uri_download = {
        .uri = "/api/capture/download", .method = HTTP_GET,
        .handler = handle_capture_download, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_download);

    const httpd_uri_t uri_delete = {
        .uri = "/api/capture/delete", .method = HTTP_POST,
        .handler = handle_capture_delete, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_delete);

    ESP_LOGI(TAG, "Capture API endpoints registered (5 routes)");
}
