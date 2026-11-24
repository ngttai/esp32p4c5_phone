idf_component_register(
    SRCS phone_app_led_rgb.cpp phone_led_rgb_icon.c
    INCLUDE_DIRS .
    REQUIRES brookesia_core phone_app_settings esp_timer
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
