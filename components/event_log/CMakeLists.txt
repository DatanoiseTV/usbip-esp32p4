idf_component_register(
    SRCS "event_log.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_psram esp_timer
)
