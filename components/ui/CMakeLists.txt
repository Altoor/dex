idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
    REQUIRES board epaper gfx qrcodegen touch_panel wifi_manager esp_system
)
