/*
 * USB Host Manager Component
 * Manages USB host stack and device enumeration
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the USB host manager
 * @return ESP_OK on success
 */
esp_err_t usb_host_mgr_init(void);

/**
 * @brief Stop the USB host manager
 * @return ESP_OK on success
 */
esp_err_t usb_host_mgr_stop(void);

#ifdef __cplusplus
}
#endif
