idf_component_register(
    SRCS "webui.c" "ws_handler.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer device_manager event_log access_control network_mgr
    EMBED_FILES
        "frontend/index.html"
        "frontend/settings.html"
        "frontend/style.css"
        "frontend/htmx.min.js"
)
