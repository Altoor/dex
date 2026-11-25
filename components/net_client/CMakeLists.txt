idf_component_register(
    SRCS "net_client.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config recorder wifi_manager esp_http_client json esp_timer mbedtls esp_netif
)
