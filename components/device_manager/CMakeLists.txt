idf_component_register(
    SRCS "device_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES event_log nvs_flash
)
