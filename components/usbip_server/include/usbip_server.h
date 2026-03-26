/*
 * USB/IP Server Component
 * TCP server for USB/IP protocol
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Connection context passed to the per-client handler task */
typedef struct {
    int       fd;         /**< Client socket file descriptor */
    uint32_t  client_ip;  /**< Client IP address (network byte order) */
} usbip_conn_ctx_t;

/**
 * @brief Initialize and start the USB/IP server
 * @return ESP_OK on success
 */
esp_err_t usbip_server_init(void);

/**
 * @brief Stop the USB/IP server
 * @return ESP_OK on success
 */
esp_err_t usbip_server_stop(void);

/**
 * @brief Per-client connection handler (runs as its own FreeRTOS task)
 * @param arg Heap-allocated usbip_conn_ctx_t; handler frees it
 */
void usbip_connection_handle(void *arg);

#ifdef __cplusplus
}
#endif
