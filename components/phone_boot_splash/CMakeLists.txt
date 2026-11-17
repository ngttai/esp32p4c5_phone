# splash.bin is built by tools/make_splash.py from tools/boot_logo.json
idf_component_register(
    SRCS phone_boot_splash.cpp
    INCLUDE_DIRS .
    REQUIRES brookesia_core
    EMBED_FILES splash.bin
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
