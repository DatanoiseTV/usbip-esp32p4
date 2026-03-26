/*
 * Event Log Component - Full Implementation
 * Ring buffer in PSRAM for system event logging
 */

#include "event_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdarg.h>

static const char *TAG = "event_log";

/** Ring buffer state */
static struct {
    event_log_entry_t *entries;    /**< Array of entries (allocated in PSRAM) */
    size_t capacity;               /**< Max entries */
    size_t head;                   /**< Next write index */
    size_t count;                  /**< Current number of valid entries */
    uint32_t total;                /**< Total events ever logged */
    SemaphoreHandle_t mutex;       /**< Thread safety */
    bool initialized;
} s_log = {0};

esp_err_t event_log_init(void)
{
    if (s_log.initialized) {
        return ESP_OK;
    }

    s_log.capacity = EVENT_LOG_MAX_ENTRIES;
    s_log.head = 0;
    s_log.count = 0;
    s_log.total = 0;

    /* Try PSRAM first, fall back to internal */
    size_t alloc_size = sizeof(event_log_entry_t) * s_log.capacity;
    s_log.entries = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_log.entries == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc failed, falling back to internal RAM");
        s_log.entries = malloc(alloc_size);
        if (s_log.entries == NULL) {
            ESP_LOGE(TAG, "Failed to allocate event log buffer");
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGI(TAG, "Event log buffer allocated in PSRAM (%u bytes)", (unsigned)alloc_size);
    }

    memset(s_log.entries, 0, alloc_size);

    s_log.mutex = xSemaphoreCreateMutex();
    if (s_log.mutex == NULL) {
        free(s_log.entries);
        s_log.entries = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_log.initialized = true;
    ESP_LOGI(TAG, "Event log initialized (capacity=%u entries)", (unsigned)s_log.capacity);

    /* Log our own init as the first event */
    event_log_add(EVENT_LOG_LEVEL_INFO, "Event log subsystem initialized");

    return ESP_OK;
}

esp_err_t event_log_add(event_log_level_t level, const char *fmt, ...)
{
    if (!s_log.initialized || fmt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_log.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    event_log_entry_t *entry = &s_log.entries[s_log.head];
    entry->timestamp_us = esp_timer_get_time();
    entry->level = level;

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->message, EVENT_LOG_MSG_MAX_LEN, fmt, args);
    va_end(args);

    s_log.head = (s_log.head + 1) % s_log.capacity;
    if (s_log.count < s_log.capacity) {
        s_log.count++;
    }
    s_log.total++;

    xSemaphoreGive(s_log.mutex);

    return ESP_OK;
}

esp_err_t event_log_get_recent(event_log_entry_t *entries, size_t max_entries, size_t *out_count)
{
    if (!s_log.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (entries == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_log.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t to_copy = (max_entries < s_log.count) ? max_entries : s_log.count;

    /* Copy most recent entries (newest first) */
    for (size_t i = 0; i < to_copy; i++) {
        size_t idx = (s_log.head + s_log.capacity - 1 - i) % s_log.capacity;
        memcpy(&entries[i], &s_log.entries[idx], sizeof(event_log_entry_t));
    }

    *out_count = to_copy;

    xSemaphoreGive(s_log.mutex);

    return ESP_OK;
}

uint32_t event_log_get_total_count(void)
{
    return s_log.total;
}
