idf_component_register(
    SRCS "usbip_server.c" "usbip_connection.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usbip_proto device_manager transfer_engine access_control event_log usb_host_mgr
)
