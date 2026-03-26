idf_component_register(
    SRCS "usb_host_mgr.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usb device_manager event_log
)
