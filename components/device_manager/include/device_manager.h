/*
 * Device Manager Component
 * USB device registry with add/remove/lookup/import/release
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of tracked USB devices */
#define DEVICE_MANAGER_MAX_DEVICES  CONFIG_USBIP_MAX_DEVICES

/** Maximum number of interfaces per device */
#define DEVICE_MANAGER_MAX_INTERFACES 16

/** Device export states */
typedef enum {
    DEV_STATE_AVAILABLE = 0,   /**< Plugged in, not exported */
    DEV_STATE_EXPORTED,        /**< Currently exported to a client */
    DEV_STATE_ERROR,           /**< Device in error state */
} device_state_t;

/** USB device speed */
typedef enum {
    DEV_SPEED_LOW = 0,
    DEV_SPEED_FULL,
    DEV_SPEED_HIGH,
    DEV_SPEED_SUPER,
} device_speed_t;

/** Per-interface descriptor info (for DEVLIST reply) */
typedef struct {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
} dm_interface_info_t;

/** Device information record */
typedef struct {
    bool     in_use;              /**< Slot is occupied */
    uint8_t  bus_id;              /**< USB bus number */
    uint8_t  dev_addr;            /**< USB device address */
    uint16_t vendor_id;           /**< USB VID */
    uint16_t product_id;         /**< USB PID */
    uint16_t bcd_device;         /**< Device release number */
    uint8_t  dev_class;          /**< USB class */
    uint8_t  dev_subclass;       /**< USB subclass */
    uint8_t  dev_protocol;       /**< USB protocol */
    uint8_t  num_configurations; /**< Number of configurations */
    uint8_t  num_interfaces;     /**< Number of interfaces in active config */
    device_speed_t speed;        /**< Device speed */
    device_state_t state;        /**< Current state */
    uint32_t client_ip;          /**< IP of importing client (if exported) */
    char     path[32];           /**< Bus ID string e.g. "1-1" */
    dm_interface_info_t interfaces[DEVICE_MANAGER_MAX_INTERFACES]; /**< Interface descriptors */
} dm_device_info_t;

/**
 * @brief Initialize the device manager
 * @return ESP_OK on success
 */
esp_err_t device_manager_init(void);

/**
 * @brief Add a newly discovered USB device
 * @param info Device information (in_use field is set automatically)
 * @param[out] out_index Index assigned to the device
 * @return ESP_OK on success, ESP_ERR_NO_MEM if registry full
 */
esp_err_t device_manager_add(const dm_device_info_t *info, int *out_index);

/**
 * @brief Remove a device from the registry
 * @param index Device index
 * @return ESP_OK on success
 */
esp_err_t device_manager_remove(int index);

/**
 * @brief Look up a device by bus_id string
 * @param path Bus ID path string (e.g. "1-1")
 * @param[out] out_index Index of the found device
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND otherwise
 */
esp_err_t device_manager_lookup(const char *path, int *out_index);

/**
 * @brief Get device info by index
 * @param index Device index
 * @param[out] out_info Copy of device info
 * @return ESP_OK on success
 */
esp_err_t device_manager_get(int index, dm_device_info_t *out_info);

/**
 * @brief Import (claim) a device for a client
 * @param index Device index
 * @param client_ip Client IP address
 * @return ESP_OK on success
 */
esp_err_t device_manager_import(int index, uint32_t client_ip);

/**
 * @brief Release an exported device
 * @param index Device index
 * @return ESP_OK on success
 */
esp_err_t device_manager_release(int index);

/**
 * @brief Release all devices exported to a specific client IP (safety cleanup)
 * @param client_ip Client IP address
 */
void device_manager_release_by_ip(uint32_t client_ip);

/**
 * @brief Get the count of currently registered devices
 * @return Number of devices
 */
int device_manager_get_count(void);

/**
 * @brief Iterate over all registered devices
 * @param callback Function called for each device (return false to stop)
 * @param user_data Opaque pointer passed to callback
 */
void device_manager_foreach(bool (*callback)(int index, const dm_device_info_t *info, void *user_data), void *user_data);

#ifdef __cplusplus
}
#endif
