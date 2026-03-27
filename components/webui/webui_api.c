/*
 * Web UI REST API Endpoints
 * Device detail, actions, settings, and system management
 */

#include "webui.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "device_manager.h"
#include "usb_host_mgr.h"
#include "access_control.h"
#include "network_mgr.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <lwip/ip4_addr.h>

static const char *TAG = "webui_api";

/* ---- Helpers ---- */

static int get_query_int(httpd_req_t *req, const char *key, int def)
{
    char qstr[64];
    if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) != ESP_OK) return def;
    char val[16];
    if (httpd_query_key_value(qstr, key, val, sizeof(val)) != ESP_OK) return def;
    return atoi(val);
}

static esp_err_t send_json(httpd_req_t *req, const char *json, int len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t send_json_ok(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true}", -1);
}

static esp_err_t send_json_error(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, (status == 404) ? "404 Not Found" :
                                (status == 400) ? "400 Bad Request" :
                                "500 Internal Server Error");
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return send_json(req, buf, len);
}

static int read_body(httpd_req_t *req, char *buf, int maxlen)
{
    int len = httpd_req_recv(req, buf, maxlen - 1);
    if (len <= 0) return -1;
    buf[len] = '\0';
    return len;
}

/* Simple JSON string value extractor: find "key":"value" and copy value into out */
static bool json_get_string(const char *json, const char *key, char *out, int outlen)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    /* skip whitespace and colon */
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outlen - 1) {
        if (*p == '\\' && *(p+1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* Simple JSON bool extractor: find "key":true/false */
static bool json_get_bool(const char *json, const char *key, bool *out)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static void json_escape(const char *src, char *dst, int dstlen)
{
    int j = 0;
    for (int i = 0; src[i] && j < dstlen - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') dst[j++] = '\\';
        if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; continue; }
        if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; continue; }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static const char *speed_str(device_speed_t s)
{
    switch (s) {
        case DEV_SPEED_LOW:   return "Low";
        case DEV_SPEED_FULL:  return "Full";
        case DEV_SPEED_HIGH:  return "High";
        case DEV_SPEED_SUPER: return "Super";
        default:              return "Unknown";
    }
}

static const char *state_str(device_state_t s)
{
    switch (s) {
        case DEV_STATE_AVAILABLE: return "Available";
        case DEV_STATE_EXPORTED:  return "Exported";
        case DEV_STATE_ERROR:     return "Error";
        default:                  return "Unknown";
    }
}

/* ---- Device detail endpoint ---- */

static esp_err_t handle_api_device_detail(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    int idx = get_query_int(req, "idx", -1);
    if (idx < 0) return send_json_error(req, 400, "missing idx");

    dm_device_info_t info;
    if (device_manager_get(idx, &info) != ESP_OK || !info.in_use) {
        return send_json_error(req, 404, "device not found");
    }

    /* Build JSON response in chunks using a large buffer */
    char *buf = malloc(4096);
    if (!buf) return send_json_error(req, 500, "out of memory");
    int pos = 0;
    int buflen = 4096;

    char mfg_esc[128], prod_esc[128], ser_esc[128];
    json_escape(info.manufacturer, mfg_esc, sizeof(mfg_esc));
    json_escape(info.product, prod_esc, sizeof(prod_esc));
    json_escape(info.serial, ser_esc, sizeof(ser_esc));

    /* Client IP as dotted string */
    char client_ip_str[20] = "";
    if (info.client_ip) {
        ip4_addr_t addr;
        addr.addr = info.client_ip;
        snprintf(client_ip_str, sizeof(client_ip_str), "%s", ip4addr_ntoa(&addr));
    }

    pos += snprintf(buf + pos, buflen - pos,
        "{\"index\":%d,\"path\":\"%s\","
        "\"vid\":%u,\"pid\":%u,\"bcd_device\":%u,"
        "\"class\":%u,\"subclass\":%u,\"protocol\":%u,"
        "\"speed\":\"%s\",\"state\":\"%s\","
        "\"client_ip\":\"%s\","
        "\"manufacturer\":\"%s\",\"product\":\"%s\",\"serial\":\"%s\","
        "\"num_configurations\":%u,\"num_interfaces\":%u,"
        "\"bytes_in\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ","
        "\"urbs_completed\":%" PRIu32 ",\"urbs_failed\":%" PRIu32 ",",
        idx, info.path,
        (unsigned)info.vendor_id, (unsigned)info.product_id, (unsigned)info.bcd_device,
        (unsigned)info.dev_class, (unsigned)info.dev_subclass, (unsigned)info.dev_protocol,
        speed_str(info.speed), state_str(info.state),
        client_ip_str,
        mfg_esc, prod_esc, ser_esc,
        (unsigned)info.num_configurations, (unsigned)info.num_interfaces,
        info.bytes_in, info.bytes_out,
        info.urbs_completed, info.urbs_failed);

    /* Parse config descriptor for interfaces and endpoints */
    pos += snprintf(buf + pos, buflen - pos, "\"interfaces\":[");
    int iface_count = 0;
    int ep_start_pos = 0; /* will store endpoint JSON separately */

    /* Temporary endpoint buffer */
    char *ep_buf = malloc(2048);
    int ep_pos = 0;
    int ep_buflen = 2048;
    int ep_count = 0;
    if (ep_buf) ep_pos += snprintf(ep_buf + ep_pos, ep_buflen - ep_pos, "\"endpoints\":[");

    if (info.config_desc_raw && info.config_desc_len > 0) {
        const uint8_t *p = info.config_desc_raw;
        const uint8_t *end = p + info.config_desc_len;
        while (p < end && p[0] > 1 && (p + p[0]) <= end) {
            uint8_t desc_len = p[0];
            uint8_t desc_type = p[1];

            if (desc_type == 0x04 && desc_len >= 9) {
                /* Interface descriptor */
                if (iface_count > 0) pos += snprintf(buf + pos, buflen - pos, ",");
                pos += snprintf(buf + pos, buflen - pos,
                    "{\"number\":%u,\"class\":%u,\"subclass\":%u,\"protocol\":%u,\"num_endpoints\":%u}",
                    p[2], p[5], p[6], p[7], p[4]);
                iface_count++;
            } else if (desc_type == 0x05 && desc_len >= 7 && ep_buf) {
                /* Endpoint descriptor */
                uint16_t max_pkt = p[4] | ((uint16_t)p[5] << 8);
                uint8_t ep_addr = p[2];
                uint8_t attr = p[3];
                const char *dir = (ep_addr & 0x80) ? "IN" : "OUT";
                const char *type_str = "Unknown";
                switch (attr & 0x03) {
                    case 0: type_str = "Control"; break;
                    case 1: type_str = "Isochronous"; break;
                    case 2: type_str = "Bulk"; break;
                    case 3: type_str = "Interrupt"; break;
                }
                if (ep_count > 0) ep_pos += snprintf(ep_buf + ep_pos, ep_buflen - ep_pos, ",");
                ep_pos += snprintf(ep_buf + ep_pos, ep_buflen - ep_pos,
                    "{\"address\":\"0x%02X\",\"direction\":\"%s\",\"type\":\"%s\","
                    "\"max_packet_size\":%u,\"interval\":%u}",
                    ep_addr, dir, type_str, max_pkt, p[6]);
                ep_count++;
            }
            p += desc_len;
        }
    }

    pos += snprintf(buf + pos, buflen - pos, "],");

    /* Append endpoints */
    if (ep_buf) {
        ep_pos += snprintf(ep_buf + ep_pos, ep_buflen - ep_pos, "]");
        pos += snprintf(buf + pos, buflen - pos, "%s", ep_buf);
        free(ep_buf);
    } else {
        pos += snprintf(buf + pos, buflen - pos, "\"endpoints\":[]");
    }

    pos += snprintf(buf + pos, buflen - pos, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, pos);
    free(buf);

    (void)ep_start_pos;
    return ESP_OK;
}

/* ---- Device action endpoints ---- */

static esp_err_t handle_api_device_reset(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    int idx = get_query_int(req, "idx", -1);
    if (idx < 0) return send_json_error(req, 400, "missing idx");

    dm_device_info_t info;
    if (device_manager_get(idx, &info) != ESP_OK || !info.in_use) {
        return send_json_error(req, 404, "device not found");
    }

    esp_err_t err = usb_host_mgr_reset_device(info.dev_addr);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "reset failed: %s", esp_err_to_name(err));
        return send_json_error(req, 500, msg);
    }

    event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s reset via WebUI", info.path);
    return send_json_ok(req);
}

static esp_err_t handle_api_device_disconnect(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    int idx = get_query_int(req, "idx", -1);
    if (idx < 0) return send_json_error(req, 400, "missing idx");

    dm_device_info_t info;
    if (device_manager_get(idx, &info) != ESP_OK || !info.in_use) {
        return send_json_error(req, 404, "device not found");
    }

    device_manager_release(idx);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s client disconnected via WebUI", info.path);
    return send_json_ok(req);
}

static esp_err_t handle_api_device_export(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    int idx = get_query_int(req, "idx", -1);
    if (idx < 0) return send_json_error(req, 400, "missing idx");

    dm_device_info_t info;
    if (device_manager_get(idx, &info) != ESP_OK || !info.in_use) {
        return send_json_error(req, 404, "device not found");
    }

    /* If exported, release it back to available */
    if (info.state == DEV_STATE_EXPORTED) {
        device_manager_release(idx);
        event_log_add(EVENT_LOG_LEVEL_INFO, "Device %s unexported via WebUI", info.path);
    }

    return send_json_ok(req);
}

/* ---- Network settings ---- */

static esp_err_t handle_api_settings_network_get(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    nvs_handle_t nvs;
    char hostname[64] = "usbip-gw";
    char dhcp[8] = "true";
    char static_ip[20] = "";
    char netmask[20] = "";
    char gateway[20] = "";
    char dns[20] = "";

    if (nvs_open("network", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(hostname);  nvs_get_str(nvs, "hostname", hostname, &len);
        len = sizeof(dhcp);      nvs_get_str(nvs, "dhcp", dhcp, &len);
        len = sizeof(static_ip); nvs_get_str(nvs, "static_ip", static_ip, &len);
        len = sizeof(netmask);   nvs_get_str(nvs, "netmask", netmask, &len);
        len = sizeof(gateway);   nvs_get_str(nvs, "gateway", gateway, &len);
        len = sizeof(dns);       nvs_get_str(nvs, "dns", dns, &len);
        nvs_close(nvs);
    }

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"hostname\":\"%s\",\"dhcp\":%s,"
        "\"static_ip\":\"%s\",\"netmask\":\"%s\","
        "\"gateway\":\"%s\",\"dns\":\"%s\"}",
        hostname, dhcp, static_ip, netmask, gateway, dns);
    return send_json(req, buf, n);
}

static esp_err_t handle_api_settings_network_post(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_json_error(req, 400, "missing body");
    }

    nvs_handle_t nvs;
    if (nvs_open("network", NVS_READWRITE, &nvs) != ESP_OK) {
        return send_json_error(req, 500, "nvs open failed");
    }

    char val[64];
    if (json_get_string(body, "hostname", val, sizeof(val)))  nvs_set_str(nvs, "hostname", val);
    if (json_get_string(body, "dhcp", val, sizeof(val)))      nvs_set_str(nvs, "dhcp", val);
    if (json_get_string(body, "static_ip", val, sizeof(val))) nvs_set_str(nvs, "static_ip", val);
    if (json_get_string(body, "netmask", val, sizeof(val)))   nvs_set_str(nvs, "netmask", val);
    if (json_get_string(body, "gateway", val, sizeof(val)))   nvs_set_str(nvs, "gateway", val);
    if (json_get_string(body, "dns", val, sizeof(val)))       nvs_set_str(nvs, "dns", val);

    nvs_commit(nvs);
    nvs_close(nvs);

    event_log_add(EVENT_LOG_LEVEL_INFO, "Network settings updated via WebUI");
    return send_json(req, "{\"ok\":true,\"reboot_required\":true}", -1);
}

/* ---- Auth settings ---- */

static esp_err_t handle_api_settings_auth_get(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    bool enabled = webui_auth_enabled();
    const char *username = webui_auth_username();

    char esc_user[128];
    json_escape(username ? username : "", esc_user, sizeof(esc_user));

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"username\":\"%s\"}",
        enabled ? "true" : "false", esc_user);
    return send_json(req, buf, n);
}

static esp_err_t handle_api_settings_auth_post(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_json_error(req, 400, "missing body");
    }

    bool enabled = false;
    char username[64] = "";
    char password[64] = "";

    json_get_bool(body, "enabled", &enabled);
    json_get_string(body, "username", username, sizeof(username));
    json_get_string(body, "password", password, sizeof(password));

    webui_auth_save(enabled, username, password);

    event_log_add(EVENT_LOG_LEVEL_INFO, "Auth settings updated via WebUI");
    return send_json_ok(req);
}

/* ---- ACL settings ---- */

static esp_err_t handle_api_settings_acl_get(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    bool closed = access_control_is_closed_mode();
    uint32_t ips[ACCESS_CONTROL_MAX_IPS];
    int count = access_control_get_allowlist(ips, ACCESS_CONTROL_MAX_IPS);

    char buf[1024];
    int pos = 0;
    int buflen = sizeof(buf);

    pos += snprintf(buf + pos, buflen - pos,
        "{\"closed\":%s,\"allowlist\":[", closed ? "true" : "false");

    for (int i = 0; i < count; i++) {
        ip4_addr_t addr;
        addr.addr = ips[i];
        if (i > 0) pos += snprintf(buf + pos, buflen - pos, ",");
        pos += snprintf(buf + pos, buflen - pos, "\"%s\"", ip4addr_ntoa(&addr));
    }

    pos += snprintf(buf + pos, buflen - pos, "]}");
    return send_json(req, buf, pos);
}

static esp_err_t handle_api_settings_acl_post(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    char body[512];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_json_error(req, 400, "missing body");
    }

    bool closed = false;
    if (json_get_bool(body, "closed", &closed)) {
        access_control_set_mode(closed);
    }

    /* Clear existing allowlist and rebuild from body */
    /* First, remove all current IPs */
    uint32_t old_ips[ACCESS_CONTROL_MAX_IPS];
    int old_count = access_control_get_allowlist(old_ips, ACCESS_CONTROL_MAX_IPS);
    for (int i = 0; i < old_count; i++) {
        access_control_remove_ip(old_ips[i]);
    }

    /* Parse allowlist array - simple approach: find each IP string in the array */
    const char *arr = strstr(body, "\"allowlist\"");
    if (arr) {
        const char *p = strchr(arr, '[');
        if (p) {
            p++;
            while (*p) {
                while (*p == ' ' || *p == ',') p++;
                if (*p == ']') break;
                if (*p == '"') {
                    p++;
                    char ip_str[20];
                    int i = 0;
                    while (*p && *p != '"' && i < (int)sizeof(ip_str) - 1) {
                        ip_str[i++] = *p++;
                    }
                    ip_str[i] = '\0';
                    if (*p == '"') p++;

                    ip4_addr_t addr;
                    if (ip4addr_aton(ip_str, &addr)) {
                        access_control_add_ip(addr.addr);
                    }
                } else {
                    break;
                }
            }
        }
    }

    event_log_add(EVENT_LOG_LEVEL_INFO, "ACL settings updated via WebUI");
    return send_json_ok(req);
}

/* ---- System info ---- */

static esp_err_t handle_api_system_info(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t internal_heap = esp_get_free_internal_heap_size();
    int64_t uptime_us = esp_timer_get_time();
    int uptime_sec = (int)(uptime_us / 1000000);

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"version\":\"1.0.0\",\"idf_version\":\"%s\","
        "\"chip\":\"ESP32-P4\",\"revision\":\"v%d.%d\","
        "\"heap_free\":%" PRIu32 ",\"heap_internal\":%" PRIu32 ","
        "\"uptime_sec\":%d}",
        esp_get_idf_version(),
        chip_info.revision / 100, chip_info.revision % 100,
        free_heap, internal_heap,
        uptime_sec);

    return send_json(req, buf, n);
}

/* ---- System restart ---- */

static esp_err_t handle_api_system_restart(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    event_log_add(EVENT_LOG_LEVEL_WARN, "System restart requested via WebUI");

    send_json(req, "{\"ok\":true,\"message\":\"restarting\"}", -1);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* ---- Factory reset ---- */

static esp_err_t handle_api_system_factory_reset(httpd_req_t *req)
{
    if (!webui_check_auth(req)) return webui_reject_auth(req);

    event_log_add(EVENT_LOG_LEVEL_WARN, "Factory reset requested via WebUI");

    send_json(req, "{\"ok\":true,\"message\":\"factory reset\"}", -1);
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_flash_erase();
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* ---- Route registration ---- */

void webui_api_register(httpd_handle_t server)
{
    /* Device detail */
    const httpd_uri_t uri_device_detail = {
        .uri = "/api/devices/detail", .method = HTTP_GET,
        .handler = handle_api_device_detail, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_device_detail);

    /* Device reset */
    const httpd_uri_t uri_device_reset = {
        .uri = "/api/devices/reset", .method = HTTP_POST,
        .handler = handle_api_device_reset, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_device_reset);

    /* Device disconnect */
    const httpd_uri_t uri_device_disconnect = {
        .uri = "/api/devices/disconnect", .method = HTTP_POST,
        .handler = handle_api_device_disconnect, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_device_disconnect);

    /* Device export toggle */
    const httpd_uri_t uri_device_export = {
        .uri = "/api/devices/export", .method = HTTP_POST,
        .handler = handle_api_device_export, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_device_export);

    /* Network settings */
    const httpd_uri_t uri_settings_net_get = {
        .uri = "/api/settings/network", .method = HTTP_GET,
        .handler = handle_api_settings_network_get, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_settings_net_get);

    const httpd_uri_t uri_settings_net_post = {
        .uri = "/api/settings/network", .method = HTTP_POST,
        .handler = handle_api_settings_network_post, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_settings_net_post);

    /* Auth settings */
    const httpd_uri_t uri_settings_auth_get = {
        .uri = "/api/settings/auth", .method = HTTP_GET,
        .handler = handle_api_settings_auth_get, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_settings_auth_get);

    const httpd_uri_t uri_settings_auth_post = {
        .uri = "/api/settings/auth", .method = HTTP_POST,
        .handler = handle_api_settings_auth_post, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_settings_auth_post);

    /* ACL settings */
    const httpd_uri_t uri_settings_acl_get = {
        .uri = "/api/settings/acl", .method = HTTP_GET,
        .handler = handle_api_settings_acl_get, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_settings_acl_get);

    const httpd_uri_t uri_settings_acl_post = {
        .uri = "/api/settings/acl", .method = HTTP_POST,
        .handler = handle_api_settings_acl_post, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_settings_acl_post);

    /* System info */
    const httpd_uri_t uri_system_info = {
        .uri = "/api/system/info", .method = HTTP_GET,
        .handler = handle_api_system_info, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_system_info);

    /* System restart */
    const httpd_uri_t uri_system_restart = {
        .uri = "/api/system/restart", .method = HTTP_POST,
        .handler = handle_api_system_restart, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_system_restart);

    /* Factory reset */
    const httpd_uri_t uri_system_factory_reset = {
        .uri = "/api/system/factory-reset", .method = HTTP_POST,
        .handler = handle_api_system_factory_reset, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_system_factory_reset);

    ESP_LOGI(TAG, "API endpoints registered (13 routes)");
}
