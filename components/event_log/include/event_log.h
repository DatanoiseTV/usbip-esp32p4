/*
 * Event Log Component
 * Ring buffer in PSRAM for system event logging
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of an event message */
#define EVENT_LOG_MSG_MAX_LEN  128

/** Maximum number of events stored in the ring buffer */
#define EVENT_LOG_MAX_ENTRIES  256

/** Event severity levels */
typedef enum {
    EVENT_LOG_LEVEL_DEBUG = 0,
    EVENT_LOG_LEVEL_INFO,
    EVENT_LOG_LEVEL_WARN,
    EVENT_LOG_LEVEL_ERROR,
} event_log_level_t;

/** A single log entry */
typedef struct {
    int64_t timestamp_us;                    /**< Timestamp in microseconds since boot */
    event_log_level_t level;                 /**< Severity level */
    char message[EVENT_LOG_MSG_MAX_LEN];     /**< Null-terminated message */
} event_log_entry_t;

/**
 * @brief Initialize the event log subsystem
 * Allocates ring buffer in PSRAM if available, otherwise in internal RAM.
 * @return ESP_OK on success
 */
esp_err_t event_log_init(void);

/**
 * @brief Add an event to the log
 * @param level Severity level
 * @param fmt printf-style format string
 * @param ... Format arguments
 * @return ESP_OK on success
 */
esp_err_t event_log_add(event_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Get recent log entries
 * @param[out] entries Array to fill with entries
 * @param max_entries Size of the entries array
 * @param[out] out_count Number of entries actually written
 * @return ESP_OK on success
 */
esp_err_t event_log_get_recent(event_log_entry_t *entries, size_t max_entries, size_t *out_count);

/**
 * @brief Get total number of events logged since init
 * @return Total event count
 */
uint32_t event_log_get_total_count(void);

#ifdef __cplusplus
}
#endif
