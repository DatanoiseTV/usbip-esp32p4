idf_component_register(
    SRCS "webui.c" "ws_handler.c" "webui_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES esp_timer device_manager event_log access_control network_mgr mbedtls nvs_flash usb_host_mgr
    EMBED_FILES
        "frontend/index.html"
        "frontend/settings.html"
        "frontend/style.css"
        "frontend/htmx.min.js"
)
