/*
 * Access Control Component
 * IP-based access control with NVS persistence
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of IPs in the allowlist */
#define ACCESS_CONTROL_MAX_IPS 32

/**
 * @brief Initialize access control subsystem
 * Loads mode and allowlist from NVS. Defaults to open mode if nothing saved.
 * @return ESP_OK on success
 */
esp_err_t access_control_init(void);

/**
 * @brief Check if a client IP is allowed
 * In open mode, always returns true. In closed mode, only allowlisted IPs pass.
 * @param client_ip Client IP address (network byte order)
 * @return true if allowed
 */
bool access_control_check(uint32_t client_ip);

/**
 * @brief Check if access control is in closed mode
 * @return true if closed mode is active
 */
bool access_control_is_closed_mode(void);

/**
 * @brief Set access control mode
 * @param closed true for closed mode, false for open mode
 */
void access_control_set_mode(bool closed);

/**
 * @brief Add an IP to the allowlist
 * Checks for duplicates and saves to NVS.
 * @param ip IP address to add (network byte order)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if list full, ESP_ERR_INVALID_STATE if duplicate
 */
esp_err_t access_control_add_ip(uint32_t ip);

/**
 * @brief Remove an IP from the allowlist
 * @param ip IP address to remove (network byte order)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not in list
 */
esp_err_t access_control_remove_ip(uint32_t ip);

/**
 * @brief Get current allowlist
 * @param[out] out_ips Array to fill with IPs
 * @param max Maximum number of IPs to return
 * @return Number of IPs written to out_ips
 */
int access_control_get_allowlist(uint32_t *out_ips, int max);

#ifdef __cplusplus
}
#endif
