idf_component_register(
    SRCS "transfer_engine.c" "transfer_iso.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES usb usb_host_mgr device_manager usbip_proto event_log
)
