idf_component_register(
    SRCS phone_app_music.cpp phone_music_icon.c music/id3_parser.cpp music/music_library.cpp music/music_service.cpp
    INCLUDE_DIRS .
    PRIV_INCLUDE_DIRS music
    REQUIRES brookesia_core esp32p4c5_bsp phone_app_settings pthread
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
