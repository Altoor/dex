idf_component_register(
    SRCS "battery.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_adc esp_driver_usb_serial_jtag
)
