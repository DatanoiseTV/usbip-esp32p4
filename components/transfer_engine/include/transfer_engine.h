/*
 * Transfer Engine Component
 * Handles USB transfer submission and completion
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the transfer engine
 * @return ESP_OK on success
 */
esp_err_t transfer_engine_init(void);

/**
 * @brief Stop the transfer engine
 * @return ESP_OK on success
 */
esp_err_t transfer_engine_stop(void);

#ifdef __cplusplus
}
#endif
