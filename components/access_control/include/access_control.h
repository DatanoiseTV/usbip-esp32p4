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

/**
 * @brief Initialize access control subsystem
 * @return ESP_OK on success
 */
esp_err_t access_control_init(void);

/**
 * @brief Check if a client IP is allowed
 * @param client_ip Client IP address (network byte order)
 * @return true if allowed
 */
bool access_control_check(uint32_t client_ip);

#ifdef __cplusplus
}
#endif
