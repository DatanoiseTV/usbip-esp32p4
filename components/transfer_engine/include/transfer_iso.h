/*
 * ISO Double-Buffer Transfer Support
 *
 * Pre-allocates two USB transfer buffers per ISO endpoint for
 * double-buffered operation: one buffer fills while the other
 * is sent over the network.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISO_MAX_PACKET_SIZE     3072    /* HS high-bandwidth ISO */
#define ISO_PACKETS_PER_URB     8
#define ISO_NUM_BUFFERS         2

typedef struct {
    usb_transfer_t *transfers[ISO_NUM_BUFFERS];
    SemaphoreHandle_t done_sems[ISO_NUM_BUFFERS];
    int active_buf;
    bool allocated;
    uint8_t ep_addr;
} iso_double_buf_t;

/**
 * @brief Allocate a double-buffered ISO transfer pair
 *
 * @param db          Pointer to the double-buffer struct (caller-owned)
 * @param ep_addr     Endpoint address
 * @param dev_handle  USB device handle
 * @param packet_size Size of each ISO packet in bytes
 * @param num_packets Number of ISO packets per URB
 * @return ESP_OK on success, ESP_ERR_NO_MEM on failure
 */
esp_err_t iso_double_buf_alloc(iso_double_buf_t *db, uint8_t ep_addr,
                               usb_device_handle_t dev_handle,
                               int packet_size, int num_packets);

/**
 * @brief Free a double-buffered ISO transfer pair
 *
 * @param db Pointer to the double-buffer struct
 */
void iso_double_buf_free(iso_double_buf_t *db);

#ifdef __cplusplus
}
#endif
