idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board epaper ui web_server wifi_manager nvs_flash
)
