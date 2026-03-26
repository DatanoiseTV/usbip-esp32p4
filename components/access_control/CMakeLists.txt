idf_component_register(
    SRCS "access_control.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash event_log
)
