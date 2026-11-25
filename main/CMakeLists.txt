idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board epaper ui web_server wifi_manager app_config i2c_bus storage
             battery recorder net_client nvs_flash esp_pm
)
