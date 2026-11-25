idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json wifi_manager esp_timer app_config battery storage net_client
    EMBED_FILES "web/index.html"
)
