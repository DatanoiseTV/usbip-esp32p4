/*
 * USB Host Manager Component
 * Manages USB host stack and device enumeration
 */
#pragma once

#include "esp_err.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the USB host manager
 *
 * Starts the USB host daemon task and class driver task.
 * The daemon installs the USB host library, and the class driver
 * registers as a client to receive device connect/disconnect events.
 *
 * @return ESP_OK on success
 */
esp_err_t usb_host_mgr_init(void);

/**
 * @brief Stop the USB host manager
 *
 * Signals both tasks to stop, waits for them to finish,
 * and uninstalls the USB host library.
 */
void usb_host_mgr_stop(void);

/**
 * @brief Get the USB device handle for a given device address
 * @param dev_addr USB device address
 * @return Device handle, or NULL if not found
 */
usb_device_handle_t usb_host_mgr_get_handle(uint8_t dev_addr);

/**
 * @brief Get the USB host client handle
 * @return Client handle used by the class driver task
 */
usb_host_client_handle_t usb_host_mgr_get_client_handle(void);

#ifdef __cplusplus
}
#endif
