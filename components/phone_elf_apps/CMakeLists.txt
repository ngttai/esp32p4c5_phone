idf_component_register(
    SRCS phone_elf_host.c phone_zip.c phone_elf_apps.cpp phone_apps_icon.c
    INCLUDE_DIRS include
    REQUIRES brookesia_core esp32p4c5_bsp phone_app_settings lvgl__lvgl esp_timer nvs_flash espressif__cjson espressif__zlib
    WHOLE_ARCHIVE
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
