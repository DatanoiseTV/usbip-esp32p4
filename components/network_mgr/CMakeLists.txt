idf_component_register(
    SRCS "network_mgr.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_eth esp_netif esp_event mdns event_log device_manager nvs_flash esp_driver_gpio
)
