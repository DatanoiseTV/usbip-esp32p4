/*
 * USB/IP Server Component
 * TCP server for USB/IP protocol
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
