/*
 * Packet Capture - Public API
 * Records USB/IP traffic to PCAP files on SD card
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture packet direction */
typedef enum {
    CAPTURE_DIR_CLIENT_TO_SERVER = 0,  /* CMD_SUBMIT, CMD_UNLINK */
    CAPTURE_DIR_SERVER_TO_CLIENT = 1,  /* RET_SUBMIT, RET_UNLINK */
} capture_direction_t;

/* Capture status (returned by capture_get_status) */
typedef struct {
    bool card_present;       /* SD card detected and mounted */
    bool capturing;          /* Currently recording */
    char filename[32];       /* Current/last capture filename */
    uint32_t file_size;      /* Bytes written to .pcap file */
    uint32_t packet_count;   /* Number of PCAP records written */
    uint32_t dropped_count;  /* Packets dropped (ring buffer full) */
    uint32_t ring_buf_used;  /* Bytes currently in ring buffer */
    uint32_t ring_buf_size;  /* Total ring buffer capacity */
} capture_status_t;

/**
 * @brief Initialize the capture subsystem (SD card, mount FAT)
 * Non-fatal if no card present.
 */
esp_err_t capture_init(void);

/**
 * @brief Start capturing to a new .pcap file
 */
esp_err_t capture_start(void);

/**
 * @brief Stop capturing and flush remaining data
 */
esp_err_t capture_stop(void);

/**
 * @brief Get current status
 */
esp_err_t capture_get_status(capture_status_t *out);

/**
 * @brief Submit a packet to the capture ring buffer.
 * Called from transfer_engine context. Non-blocking.
 * If ring buffer is full, packet is silently dropped.
 *
 * @param dir       Direction (client->server or server->client)
 * @param hdr       USB/IP header in host byte order (48 bytes)
 * @param hdr_len   Size of header (sizeof(usbip_header_t))
 * @param payload   Transfer data payload (may be NULL)
 * @param payload_len Length of payload data
 */
void capture_submit_packet(capture_direction_t dir,
                           const void *hdr, uint32_t hdr_len,
                           const void *payload, uint32_t payload_len);

/**
 * @brief Delete the current/last capture file
 */
esp_err_t capture_delete_file(void);

/**
 * @brief Get the full filesystem path of the current capture file
 */
const char *capture_get_filepath(void);

/**
 * @brief Register HTTP API endpoints on the given server
 */
void capture_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
