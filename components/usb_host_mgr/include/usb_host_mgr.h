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

/**
 * @brief Claim all interfaces of a device
 *
 * Must be called before submitting non-control transfers.
 * @param dev_addr USB device address
 * @return ESP_OK on success
 */
esp_err_t usb_host_mgr_claim_interfaces(uint8_t dev_addr);

/**
 * @brief Release all claimed interfaces of a device
 * @param dev_addr USB device address
 * @return ESP_OK on success
 */
esp_err_t usb_host_mgr_release_interfaces(uint8_t dev_addr);

/**
 * @brief Notify that a device has been physically removed
 *
 * Called from handle_device_gone() so the transfer engine can detect
 * removal without polling device_manager.
 *
 * @param dev_addr USB device address that was removed
 */
void usb_host_mgr_notify_removal(uint8_t dev_addr);

/**
 * @brief Check and clear the device-removal flag
 *
 * @return The dev_addr that was removed, or 0 if none
 */
uint8_t usb_host_mgr_check_removal(void);

/**
 * @brief Reset a USB device
 *
 * Currently returns ESP_ERR_NOT_SUPPORTED as the underlying
 * usb_host_device_reset() API is not available in ESP-IDF v5.5.
 *
 * @param dev_addr USB device address
 * @return ESP_OK on success, or ESP_ERR_NOT_SUPPORTED
 */
esp_err_t usb_host_mgr_reset_device(uint8_t dev_addr);

#ifdef __cplusplus
}
#endif
