/*
 * Web UI Component
 * HTTP server with embedded frontend
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the web UI server
 * @return ESP_OK on success
 */
esp_err_t webui_init(void);

/**
 * @brief Stop the web UI server
 * @return ESP_OK on success
 */
esp_err_t webui_stop(void);

#ifdef __cplusplus
}
#endif
