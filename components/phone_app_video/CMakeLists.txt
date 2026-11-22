idf_component_register(
    SRCS phone_app_video.cpp phone_video_icon.c phone_video_h264mem.cpp
    INCLUDE_DIRS .
    REQUIRES brookesia_core esp32p4c5_bsp phone_app_settings esp_driver_jpeg esp_driver_ppa esp_timer espressif__esp_h264 espressif__esp_extractor espressif__esp_audio_codec esp_ringbuf
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# H.264 decoder picture buffers in PSRAM (phone_video_h264mem.cpp)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_malloc_prefer" "-Wl,--wrap=heap_caps_calloc_prefer")
