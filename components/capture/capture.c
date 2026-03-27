/*
 * Packet Capture - Core Implementation
 * SD card init, PCAP file I/O, ring buffer, writer task
 */

#include "capture.h"
#include "usbip_proto.h"
#include "event_log.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "capture";

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define MOUNT_POINT     "/sdcard"
#define SDMMC_CLK_GPIO  43
#define SDMMC_CMD_GPIO  44
#define SDMMC_D0_GPIO   39
#define SDMMC_D1_GPIO   40
#define SDMMC_D2_GPIO   41
#define SDMMC_D3_GPIO   42

#define PCAP_MAGIC      0xa1b2c3d4
#define PCAP_VERSION_MAJOR  2
#define PCAP_VERSION_MINOR  4
#define PCAP_LINKTYPE   220  /* LINKTYPE_USB_LINUX_MMAPPED */
#define PCAP_SNAPLEN    65535

#define MON_BIN_HDR_SIZE  64

/* Writer task config */
#define WRITER_TASK_STACK   4096
#define WRITER_TASK_PRIO    2
#define WRITER_TASK_CORE    1
#define WRITER_POLL_MS      10

/* ------------------------------------------------------------------ */
/* PCAP structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
} pcap_global_hdr_t;

_Static_assert(sizeof(pcap_global_hdr_t) == 24, "pcap_global_hdr must be 24 bytes");

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcap_rec_hdr_t;

_Static_assert(sizeof(pcap_rec_hdr_t) == 16, "pcap_rec_hdr must be 16 bytes");

typedef struct __attribute__((packed)) {
    uint64_t id;
    uint8_t  type;
    uint8_t  xfer_type;
    uint8_t  ep_addr;
    uint8_t  devnum;
    uint16_t busnum;
    uint8_t  flag_setup;
    uint8_t  flag_data;
    int64_t  ts_sec;
    int32_t  ts_usec;
    int32_t  status;
    uint32_t urb_len;
    uint32_t data_len;
    uint8_t  setup[8];
    int32_t  interval;
    int32_t  start_frame;
    uint32_t xfer_flags;
    uint32_t ndesc;
} mon_bin_hdr_t;

_Static_assert(sizeof(mon_bin_hdr_t) == 64, "mon_bin_hdr must be 64 bytes");

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    uint32_t size;
    volatile uint32_t head;
    volatile uint32_t tail;
    portMUX_TYPE lock;
} capture_ring_t;

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static bool s_initialized = false;
static bool s_card_present = false;
static bool s_capturing = false;
static char s_filename[32] = "";
static char s_filepath[64] = "";
static uint32_t s_file_size = 0;
static uint32_t s_packet_count = 0;
static uint32_t s_dropped_count = 0;
static uint16_t s_file_counter = 0;

static capture_ring_t s_ring;
static FILE *s_pcap_file = NULL;
static TaskHandle_t s_writer_task = NULL;
static volatile bool s_writer_running = false;

static sdmmc_card_t *s_card = NULL;

/* ------------------------------------------------------------------ */
/* Ring buffer helpers                                                  */
/* ------------------------------------------------------------------ */

static inline uint32_t ring_used(const capture_ring_t *r)
{
    uint32_t h = r->head;
    uint32_t t = r->tail;
    if (h >= t) return h - t;
    return r->size - t + h;
}

static inline uint32_t ring_free(const capture_ring_t *r)
{
    /* Reserve 1 byte to distinguish full from empty */
    return r->size - ring_used(r) - 1;
}

/**
 * @brief Write data to ring buffer with wrap-around handling.
 * Caller must hold spinlock and ensure enough space.
 */
static void ring_write(capture_ring_t *r, const void *data, uint32_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    uint32_t pos = r->head;
    uint32_t first = r->size - pos;

    if (first >= len) {
        memcpy(r->buf + pos, src, len);
    } else {
        memcpy(r->buf + pos, src, first);
        memcpy(r->buf, src + first, len - first);
    }
    r->head = (pos + len) % r->size;
}

/**
 * @brief Read data from ring buffer with wrap-around handling.
 * Caller must hold spinlock and ensure enough data.
 */
static void ring_read(capture_ring_t *r, void *data, uint32_t len)
{
    uint8_t *dst = (uint8_t *)data;
    uint32_t pos = r->tail;
    uint32_t first = r->size - pos;

    if (first >= len) {
        memcpy(dst, r->buf + pos, len);
    } else {
        memcpy(dst, r->buf + pos, first);
        memcpy(dst + first, r->buf, len - first);
    }
    r->tail = (pos + len) % r->size;
}

/**
 * @brief Peek data from ring buffer without advancing tail.
 */
static void ring_peek(const capture_ring_t *r, void *data, uint32_t len)
{
    uint8_t *dst = (uint8_t *)data;
    uint32_t pos = r->tail;
    uint32_t first = r->size - pos;

    if (first >= len) {
        memcpy(dst, r->buf + pos, len);
    } else {
        memcpy(dst, r->buf + pos, first);
        memcpy(dst + first, r->buf, len - first);
    }
}

/* ------------------------------------------------------------------ */
/* SD card init / mount                                                */
/* ------------------------------------------------------------------ */

static esp_err_t sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    /* ESP32-P4 Slot 0 (GPIO39-48) requires internal LDO to power VDD_IO_5.
     * LDO channel 4 provides the IO voltage for these pins. */
#if SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ldo_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ldo_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SD power control LDO: %s", esp_err_to_name(ldo_err));
        return ldo_err;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = (gpio_num_t)SDMMC_CLK_GPIO;
    slot.cmd = (gpio_num_t)SDMMC_CMD_GPIO;
    slot.d0  = (gpio_num_t)SDMMC_D0_GPIO;
    slot.d1  = (gpio_num_t)SDMMC_D1_GPIO;
    slot.d2  = (gpio_num_t)SDMMC_D2_GPIO;
    slot.d3  = (gpio_num_t)SDMMC_D3_GPIO;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted: %s, %.1f MB",
             s_card->cid.name,
             (float)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024.0f * 1024.0f));
    return ESP_OK;
}

static void sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* File naming                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Scan for next available capture file number.
 */
static uint16_t find_next_file_number(void)
{
    uint16_t max_found = 0;
    bool found_any = false;
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cap_", 4) == 0) {
            int num = atoi(ent->d_name + 4);
            if (num >= 0 && (uint16_t)num >= max_found) {
                max_found = (uint16_t)num;
                found_any = true;
            }
        }
    }
    closedir(dir);

    return found_any ? (uint16_t)((max_found + 1) % 10000) : 0;
}

/* ------------------------------------------------------------------ */
/* PCAP writing                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t write_pcap_global_header(FILE *f)
{
    pcap_global_hdr_t ghdr = {
        .magic_number  = PCAP_MAGIC,
        .version_major = PCAP_VERSION_MAJOR,
        .version_minor = PCAP_VERSION_MINOR,
        .thiszone      = 0,
        .sigfigs       = 0,
        .snaplen       = PCAP_SNAPLEN,
        .network       = PCAP_LINKTYPE,
    };
    if (fwrite(&ghdr, sizeof(ghdr), 1, f) != 1) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Writer task                                                         */
/* ------------------------------------------------------------------ */

static void capture_writer_task(void *arg)
{
    ESP_LOGI(TAG, "Writer task started");

    while (s_writer_running) {
        if (ring_used(&s_ring) == 0) {
            vTaskDelay(pdMS_TO_TICKS(WRITER_POLL_MS));
            continue;
        }

        /* Read entry_len from ring buffer */
        uint32_t entry_len = 0;

        taskENTER_CRITICAL(&s_ring.lock);
        uint32_t used = ring_used(&s_ring);
        if (used < 4) {
            taskEXIT_CRITICAL(&s_ring.lock);
            vTaskDelay(pdMS_TO_TICKS(WRITER_POLL_MS));
            continue;
        }
        ring_peek(&s_ring, &entry_len, 4);
        taskEXIT_CRITICAL(&s_ring.lock);

        /* Validate entry_len */
        if (entry_len < sizeof(pcap_rec_hdr_t) + MON_BIN_HDR_SIZE || entry_len > 4096) {
            ESP_LOGE(TAG, "Invalid ring entry_len: %lu, resetting ring", (unsigned long)entry_len);
            taskENTER_CRITICAL(&s_ring.lock);
            s_ring.head = 0;
            s_ring.tail = 0;
            taskEXIT_CRITICAL(&s_ring.lock);
            continue;
        }

        /* Wait until the full entry is available */
        taskENTER_CRITICAL(&s_ring.lock);
        used = ring_used(&s_ring);
        if (used < 4 + entry_len) {
            taskEXIT_CRITICAL(&s_ring.lock);
            vTaskDelay(pdMS_TO_TICKS(WRITER_POLL_MS));
            continue;
        }

        /* Read the entry: skip the 4-byte length prefix, read payload */
        uint8_t tmp[4];
        ring_read(&s_ring, tmp, 4);  /* consume entry_len */

        /* Read the actual data onto stack (bounded by entry_len <= ~384 bytes) */
        uint8_t entry_buf[512];
        uint32_t to_read = entry_len;
        if (to_read > sizeof(entry_buf)) to_read = sizeof(entry_buf);
        ring_read(&s_ring, entry_buf, to_read);

        /* If entry was bigger than our buffer (shouldn't happen), skip rest */
        if (entry_len > to_read) {
            uint32_t skip = entry_len - to_read;
            while (skip > 0) {
                uint32_t chunk = skip > sizeof(tmp) ? sizeof(tmp) : skip;
                ring_read(&s_ring, tmp, chunk);
                skip -= chunk;
            }
        }
        taskEXIT_CRITICAL(&s_ring.lock);

        /* Write to file */
        if (s_pcap_file) {
            size_t written = fwrite(entry_buf, 1, to_read, s_pcap_file);
            if (written != to_read) {
                ESP_LOGE(TAG, "SD card write error, stopping capture");
                s_card_present = false;
                s_capturing = false;
                s_writer_running = false;
                event_log_add(EVENT_LOG_LEVEL_ERROR, "SD card write error, capture stopped");
                break;
            }
            s_file_size += written;
            s_packet_count++;
        }
    }

    /* Flush and close file */
    if (s_pcap_file) {
        fflush(s_pcap_file);
        fclose(s_pcap_file);
        s_pcap_file = NULL;
    }

    ESP_LOGI(TAG, "Writer task exiting");
    s_writer_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t capture_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Allocate ring buffer in PSRAM */
    uint32_t ring_size = CONFIG_CAPTURE_RING_BUF_SIZE;
    s_ring.buf = (uint8_t *)heap_caps_malloc(ring_size, MALLOC_CAP_SPIRAM);
    if (!s_ring.buf) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer (%lu bytes in PSRAM)",
                 (unsigned long)ring_size);
        return ESP_ERR_NO_MEM;
    }
    s_ring.size = ring_size;
    s_ring.head = 0;
    s_ring.tail = 0;
    portMUX_INITIALIZE(&s_ring.lock);

    ESP_LOGI(TAG, "Ring buffer: %lu bytes in PSRAM", (unsigned long)ring_size);

    /* Try to mount SD card */
    if (sd_mount() == ESP_OK) {
        s_card_present = true;
        s_file_counter = find_next_file_number();
        ESP_LOGI(TAG, "Next capture file number: %u", s_file_counter);
    } else {
        s_card_present = false;
        ESP_LOGW(TAG, "No SD card, capture disabled until card inserted");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Capture subsystem initialized");
    return ESP_OK;
}

esp_err_t capture_start(void)
{
    if (s_capturing) {
        ESP_LOGW(TAG, "Already capturing");
        return ESP_ERR_INVALID_STATE;
    }

    /* Try re-init SD if not present */
    if (!s_card_present) {
        sd_unmount();
        if (sd_mount() == ESP_OK) {
            s_card_present = true;
            s_file_counter = find_next_file_number();
        } else {
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Reset counters */
    s_file_size = 0;
    s_packet_count = 0;
    s_dropped_count = 0;

    /* Reset ring buffer */
    taskENTER_CRITICAL(&s_ring.lock);
    s_ring.head = 0;
    s_ring.tail = 0;
    taskEXIT_CRITICAL(&s_ring.lock);

    /* Generate filename */
    snprintf(s_filename, sizeof(s_filename), "cap_%04u.pcap", s_file_counter);
    snprintf(s_filepath, sizeof(s_filepath), MOUNT_POINT "/%s", s_filename);
    s_file_counter = (s_file_counter + 1) % 10000;

    /* Open file and write PCAP global header */
    s_pcap_file = fopen(s_filepath, "wb");
    if (!s_pcap_file) {
        ESP_LOGE(TAG, "Failed to open %s for writing", s_filepath);
        return ESP_FAIL;
    }

    if (write_pcap_global_header(s_pcap_file) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write PCAP header");
        fclose(s_pcap_file);
        s_pcap_file = NULL;
        return ESP_FAIL;
    }
    s_file_size = sizeof(pcap_global_hdr_t);
    fflush(s_pcap_file);

    /* Start writer task */
    s_capturing = true;
    s_writer_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        capture_writer_task, "cap_writer",
        WRITER_TASK_STACK, NULL,
        WRITER_TASK_PRIO, &s_writer_task,
        WRITER_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create writer task");
        s_capturing = false;
        s_writer_running = false;
        fclose(s_pcap_file);
        s_pcap_file = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Capture started: %s", s_filepath);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Capture started: %s", s_filename);
    return ESP_OK;
}

esp_err_t capture_stop(void)
{
    if (!s_capturing) {
        return ESP_OK;
    }

    s_capturing = false;
    s_writer_running = false;

    /* Wait for writer task to finish */
    if (s_writer_task) {
        /* Give it time to flush and exit */
        for (int i = 0; i < 100 && s_writer_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "Capture stopped: %s (%lu bytes, %lu packets, %lu dropped)",
             s_filename, (unsigned long)s_file_size,
             (unsigned long)s_packet_count, (unsigned long)s_dropped_count);
    event_log_add(EVENT_LOG_LEVEL_INFO, "Capture stopped: %s (%lu pkts, %lu dropped)",
                  s_filename, (unsigned long)s_packet_count, (unsigned long)s_dropped_count);
    return ESP_OK;
}

esp_err_t capture_get_status(capture_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->card_present = s_card_present;
    out->capturing = s_capturing;
    strncpy(out->filename, s_filename, sizeof(out->filename) - 1);
    out->file_size = s_file_size;
    out->packet_count = s_packet_count;
    out->dropped_count = s_dropped_count;
    out->ring_buf_used = ring_used(&s_ring);
    out->ring_buf_size = s_ring.size;
    return ESP_OK;
}

/**
 * @brief Build a mon_bin_hdr from a USB/IP header (host byte order).
 */
static void build_mon_hdr(mon_bin_hdr_t *mon, capture_direction_t dir,
                          const usbip_header_t *usbip_hdr,
                          uint32_t captured_data_len, int64_t now_us)
{
    memset(mon, 0, sizeof(*mon));

    uint32_t cmd = usbip_hdr->base.command;
    uint32_t ep  = usbip_hdr->base.ep;

    mon->id = (uint64_t)usbip_hdr->base.seqnum;
    mon->busnum = 1;

    /* Determine type ('S' for submit/request, 'C' for complete/response) */
    if (cmd == USBIP_CMD_SUBMIT || cmd == USBIP_CMD_UNLINK) {
        mon->type = 'S';
    } else {
        mon->type = 'C';
    }

    /* Transfer type: if ep==0 -> CTRL(2), else default to BULK(3) */
    if (ep == 0) {
        mon->xfer_type = 2; /* CTRL */
    } else {
        mon->xfer_type = 3; /* BULK (default; we don't know the actual type) */
    }

    /* Endpoint address with direction bit */
    if (cmd == USBIP_CMD_SUBMIT || cmd == USBIP_RET_SUBMIT) {
        mon->ep_addr = (uint8_t)(ep | (usbip_hdr->base.direction == USBIP_DIR_IN ? 0x80 : 0x00));
    } else {
        /* UNLINK: ep=0 */
        mon->ep_addr = 0;
    }

    /* Device number: extract from devid (devid = busnum << 16 | devnum) */
    mon->devnum = (uint8_t)(usbip_hdr->base.devid & 0xFF);

    /* Timestamp */
    mon->ts_sec  = now_us / 1000000;
    mon->ts_usec = (int32_t)(now_us % 1000000);

    /* Command-specific fields */
    switch (cmd) {
    case USBIP_CMD_SUBMIT:
        mon->status = 0;
        mon->urb_len = (uint32_t)usbip_hdr->u.cmd_submit.transfer_buffer_length;
        mon->data_len = captured_data_len;
        mon->xfer_flags = usbip_hdr->u.cmd_submit.transfer_flags;
        mon->interval = usbip_hdr->u.cmd_submit.interval;
        mon->start_frame = usbip_hdr->u.cmd_submit.start_frame;
        /* Setup packet for control */
        if (ep == 0) {
            memcpy(mon->setup, usbip_hdr->u.cmd_submit.setup, 8);
            mon->flag_setup = 0;    /* setup present */
        } else {
            mon->flag_setup = 0xFF; /* no setup */
        }
        /* ISO packets */
        if (usbip_hdr->u.cmd_submit.number_of_packets != (int32_t)0xFFFFFFFF &&
            usbip_hdr->u.cmd_submit.number_of_packets > 0) {
            mon->ndesc = (uint32_t)usbip_hdr->u.cmd_submit.number_of_packets;
        }
        break;

    case USBIP_RET_SUBMIT:
        mon->status = usbip_hdr->u.ret_submit.status;
        mon->urb_len = (uint32_t)usbip_hdr->u.ret_submit.actual_length;
        mon->data_len = captured_data_len;
        mon->flag_setup = 0xFF; /* no setup in response */
        if (usbip_hdr->u.ret_submit.number_of_packets != (int32_t)0xFFFFFFFF &&
            usbip_hdr->u.ret_submit.number_of_packets > 0) {
            mon->ndesc = (uint32_t)usbip_hdr->u.ret_submit.number_of_packets;
        }
        break;

    case USBIP_CMD_UNLINK:
        mon->status = 0;
        mon->urb_len = 0;
        mon->data_len = 0;
        mon->flag_setup = 0xFF;
        break;

    case USBIP_RET_UNLINK:
        mon->status = usbip_hdr->u.ret_unlink.status;
        mon->urb_len = 0;
        mon->data_len = 0;
        mon->flag_setup = 0xFF;
        break;

    default:
        break;
    }

    /* Data flag */
    mon->flag_data = (captured_data_len > 0) ? 0 : 1;
}

void capture_submit_packet(capture_direction_t dir,
                           const void *hdr, uint32_t hdr_len,
                           const void *payload, uint32_t payload_len)
{
    if (!s_capturing || !s_ring.buf) {
        return;
    }

    /* Sanity check header size */
    if (hdr_len < sizeof(usbip_header_t) || !hdr) {
        return;
    }

    const usbip_header_t *usbip_hdr = (const usbip_header_t *)hdr;

    /* Truncate payload to snap length */
    uint32_t snap = CONFIG_CAPTURE_SNAPLEN;
    uint32_t captured_payload = payload_len;
    if (captured_payload > snap) {
        captured_payload = snap;
    }
    if (!payload) {
        captured_payload = 0;
    }

    /* Build PCAP record header */
    int64_t now_us = esp_timer_get_time();
    uint32_t pcap_data_len = MON_BIN_HDR_SIZE + captured_payload;
    uint32_t pcap_orig_len = MON_BIN_HDR_SIZE + payload_len;

    pcap_rec_hdr_t rec_hdr = {
        .ts_sec  = (uint32_t)(now_us / 1000000),
        .ts_usec = (uint32_t)(now_us % 1000000),
        .incl_len = pcap_data_len,
        .orig_len = pcap_orig_len,
    };

    /* Build mon_bin_hdr */
    mon_bin_hdr_t mon;
    build_mon_hdr(&mon, dir, usbip_hdr, captured_payload, now_us);

    /* Total entry size: 4 (entry_len) + 16 (pcap_rec) + 64 (mon) + payload */
    uint32_t entry_data_len = sizeof(pcap_rec_hdr_t) + MON_BIN_HDR_SIZE + captured_payload;
    uint32_t total_entry = 4 + entry_data_len;

    /* Write to ring buffer under spinlock */
    taskENTER_CRITICAL(&s_ring.lock);

    if (ring_free(&s_ring) < total_entry) {
        s_dropped_count++;
        taskEXIT_CRITICAL(&s_ring.lock);
        return;
    }

    ring_write(&s_ring, &entry_data_len, 4);
    ring_write(&s_ring, &rec_hdr, sizeof(rec_hdr));
    ring_write(&s_ring, &mon, MON_BIN_HDR_SIZE);
    if (captured_payload > 0 && payload) {
        ring_write(&s_ring, payload, captured_payload);
    }

    taskEXIT_CRITICAL(&s_ring.lock);
}

esp_err_t capture_delete_file(void)
{
    if (s_capturing) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_filepath[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    if (remove(s_filepath) == 0) {
        ESP_LOGI(TAG, "Deleted %s", s_filepath);
        s_filename[0] = '\0';
        s_filepath[0] = '\0';
        s_file_size = 0;
        s_packet_count = 0;
        s_dropped_count = 0;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to delete %s", s_filepath);
    return ESP_FAIL;
}

const char *capture_get_filepath(void)
{
    return s_filepath;
}
