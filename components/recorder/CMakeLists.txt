idf_component_register(
    SRCS "recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES board storage i2c_bus esp_codec_dev esp_driver_i2s esp_driver_i2c esp_driver_gpio heap
)
