idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        network_mgr
        usb_host_mgr
        device_manager
        usbip_server
        webui
        event_log
        access_control
        nvs_flash
        esp_psram
)
