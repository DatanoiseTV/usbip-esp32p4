/*
 * Isochronous Transfer Handler - Double-Buffered ISO Transfers
 *
 * Pre-allocates two USB transfer buffers per ISO endpoint so that while one
 * buffer is being filled by the USB controller, the previous completed buffer
 * can be sent over the network.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "transfer_iso.h"

static const char *TAG = "xfer_iso";

esp_err_t iso_double_buf_alloc(iso_double_buf_t *db, uint8_t ep_addr,
                               usb_device_handle_t dev_handle,
                               int packet_size, int num_packets)
{
    /* 1. Zero the struct */
    memset(db, 0, sizeof(*db));

    /* 2. Allocate each buffer */
    for (int i = 0; i < ISO_NUM_BUFFERS; i++) {
        /* Create binary semaphore */
        db->done_sems[i] = xSemaphoreCreateBinary();
        if (db->done_sems[i] == NULL) {
            ESP_LOGE(TAG, "Failed to create semaphore for buffer %d", i);
            goto fail;
        }

        /* Allocate USB transfer */
        size_t buf_size = (size_t)packet_size * (size_t)num_packets;
        esp_err_t err = usb_host_transfer_alloc(buf_size, num_packets, &db->transfers[i]);
        if (err != ESP_OK || db->transfers[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate transfer for buffer %d: %s",
                     i, esp_err_to_name(err));
            goto fail;
        }

        /* Configure the transfer */
        usb_transfer_t *xfer = db->transfers[i];
        xfer->device_handle = dev_handle;
        xfer->bEndpointAddress = ep_addr;
        /* num_isoc_packets is const, set by usb_host_transfer_alloc */
        xfer->num_bytes = (int)buf_size;

        /* Set each isochronous packet descriptor */
        for (int j = 0; j < num_packets; j++) {
            xfer->isoc_packet_desc[j].num_bytes = packet_size;
        }
    }

    db->ep_addr = ep_addr;
    db->active_buf = 0;
    db->allocated = true;

    ESP_LOGI(TAG, "ISO double buffer allocated: ep=0x%02x pkt_size=%d num_pkts=%d",
             ep_addr, packet_size, num_packets);
    return ESP_OK;

fail:
    /* Free everything already allocated */
    for (int i = 0; i < ISO_NUM_BUFFERS; i++) {
        if (db->transfers[i] != NULL) {
            usb_host_transfer_free(db->transfers[i]);
            db->transfers[i] = NULL;
        }
        if (db->done_sems[i] != NULL) {
            vSemaphoreDelete(db->done_sems[i]);
            db->done_sems[i] = NULL;
        }
    }
    memset(db, 0, sizeof(*db));
    return ESP_ERR_NO_MEM;
}

void iso_double_buf_free(iso_double_buf_t *db)
{
    if (!db->allocated) {
        return;
    }

    for (int i = 0; i < ISO_NUM_BUFFERS; i++) {
        if (db->transfers[i] != NULL) {
            usb_host_transfer_free(db->transfers[i]);
        }
        if (db->done_sems[i] != NULL) {
            vSemaphoreDelete(db->done_sems[i]);
        }
    }

    memset(db, 0, sizeof(*db));
    ESP_LOGI(TAG, "ISO double buffer freed");
}
