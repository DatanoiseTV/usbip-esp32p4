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

/**
 * @brief Run the URB transfer loop for an imported device
 * Blocks until the connection is closed or an error occurs.
 * @param sockfd Client socket file descriptor
 * @param busid  Bus ID string of the imported device
 * @return 0 on clean disconnect, -1 on error
 */
int transfer_engine_run(int sockfd, const char *busid);

#ifdef __cplusplus
}
#endif
