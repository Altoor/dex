idf_component_register(
    SRCS "touch.c"
    INCLUDE_DIRS "include"
    REQUIRES board i2c_bus esp_driver_i2c esp_driver_gpio
)
