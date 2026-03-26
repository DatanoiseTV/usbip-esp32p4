/*
 * Network Manager Component
 * Ethernet, IP, and mDNS management
 */
#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize network stack (Ethernet + mDNS)
 * @return ESP_OK on success
 */
esp_err_t network_mgr_init(void);

/**
 * @brief Get the Ethernet network interface
 * @return Pointer to esp_netif_t, or NULL if not initialized
 */
esp_netif_t *network_mgr_get_netif(void);

/**
 * @brief Update the mDNS TXT record for device count
 * @param device_count Number of currently attached USB devices
 */
void network_mgr_update_mdns_devices(int device_count);

/**
 * @brief Get current IP address as string
 * @param[out] buf Buffer to write IP string
 * @param buf_len Length of buffer
 * @return ESP_OK on success
 */
esp_err_t network_mgr_get_ip_str(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
