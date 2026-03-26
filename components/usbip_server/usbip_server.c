/*
 * USB/IP Server - TCP Listener
 * Listens on CONFIG_USBIP_TCP_PORT, accepts clients, spawns connection handlers.
 */

#include "usbip_server.h"
#include "access_control.h"
#include "usbip_proto.h"
#include "event_log.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "usbip_srv";

static TaskHandle_t s_server_task = NULL;
static int          s_listen_fd   = -1;
static volatile bool s_running    = false;

/* Format an IPv4 address (network byte order) into a dotted string */
static void ip4_to_str(uint32_t ip_nbo, char *buf, size_t buflen)
{
    const uint8_t *b = (const uint8_t *)&ip_nbo;
    snprintf(buf, buflen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void usbip_server_task(void *arg)
{
    (void)arg;

    /* Create TCP socket */
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno=%d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* SO_REUSEADDR */
    int opt = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_USBIP_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno=%d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Listen */
    if (listen(s_listen_fd, 4) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno=%d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB/IP server listening on port %d", CONFIG_USBIP_TCP_PORT);
    event_log_add(EVENT_LOG_LEVEL_INFO, "USB/IP server started on port %d", CONFIG_USBIP_TCP_PORT);

    /* Accept loop */
    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(s_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (s_running) {
                ESP_LOGE(TAG, "Accept failed: errno=%d", errno);
            }
            continue;
        }

        uint32_t client_ip = client_addr.sin_addr.s_addr;
        char ip_str[16];
        ip4_to_str(client_ip, ip_str, sizeof(ip_str));

        /* Access control check */
        if (!access_control_check(client_ip)) {
            ESP_LOGW(TAG, "Connection from %s denied by access control", ip_str);
            event_log_add(EVENT_LOG_LEVEL_WARN, "Rejected connection from %s", ip_str);
            close(client_fd);
            continue;
        }

        ESP_LOGI(TAG, "Accepted connection from %s (fd=%d)", ip_str, client_fd);
        event_log_add(EVENT_LOG_LEVEL_INFO, "Client connected: %s", ip_str);

        /* Configure socket (TCP_NODELAY, keepalive, etc.) */
        usbip_net_configure_socket(client_fd);

        /* Heap-allocate context for the handler task */
        usbip_conn_ctx_t *ctx = malloc(sizeof(usbip_conn_ctx_t));
        if (ctx == NULL) {
            ESP_LOGE(TAG, "Failed to allocate connection context");
            close(client_fd);
            continue;
        }
        ctx->fd = client_fd;
        ctx->client_ip = client_ip;

        /* Spawn connection handler task */
        char task_name[20];
        snprintf(task_name, sizeof(task_name), "usbip_%d", client_fd);

        BaseType_t ret = xTaskCreatePinnedToCore(
            usbip_connection_handle,
            task_name,
            8192,
            ctx,
            19,
            NULL,
            1  /* Core 1 */
        );

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create connection handler task");
            free(ctx);
            close(client_fd);
        }
    }

    /* Cleanup */
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    ESP_LOGI(TAG, "USB/IP server task exiting");
    s_server_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t usbip_server_init(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    s_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        usbip_server_task,
        "usbip_srv",
        4096,
        NULL,
        18,
        &s_server_task,
        1  /* Core 1 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create server task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "USB/IP server initialized");
    return ESP_OK;
}

esp_err_t usbip_server_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping USB/IP server...");
    s_running = false;

    /* Close the listen socket to unblock accept() */
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }

    /* Give the task time to exit */
    for (int i = 0; i < 20 && s_server_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "USB/IP server stopped");
    event_log_add(EVENT_LOG_LEVEL_INFO, "USB/IP server stopped");
    return ESP_OK;
}
