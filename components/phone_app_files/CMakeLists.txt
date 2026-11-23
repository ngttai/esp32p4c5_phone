idf_component_register(
    SRCS phone_app_files.cpp phone_files_icon.c
    INCLUDE_DIRS .
    REQUIRES brookesia_core esp32p4c5_bsp phone_app_settings esp_timer
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# after eject from the PC the drive reports no medium (see phone_app_files.cpp)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=tud_msc_test_unit_ready_cb")
