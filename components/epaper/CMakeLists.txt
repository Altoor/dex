idf_component_register(
    SRCS "epaper.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_spi esp_driver_gpio
)
