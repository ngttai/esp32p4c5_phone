file(GLOB FONT_SRCS fonts/*.c)

idf_component_register(
    SRCS phone_app_settings.cpp phone_settings_icon.c phone_net.cpp phone_locale.cpp phone_power.cpp phone_audio.cpp phone_storage.cpp phone_lock.cpp phone_volume.cpp
         phone_fonts.c ${FONT_SRCS}
    INCLUDE_DIRS .
    REQUIRES brookesia_core esp32p4c5_bsp esp_app_format fatfs esp_wifi esp_netif esp_event esp_timer nvs_flash
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
