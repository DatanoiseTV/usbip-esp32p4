idf_component_register(
    SRCS "capture.c" "capture_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES sdmmc fatfs esp_timer vfs esp_driver_sdmmc esp_hw_support webui event_log usbip_proto
)
