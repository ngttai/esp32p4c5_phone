idf_component_register(
    SRCS phone_elf_test.c
    INCLUDE_DIRS include
    REQUIRES esp32p4c5_bsp phone_app_settings lvgl__lvgl esp_timer nvs_flash
    EMBED_FILES app_test.elf
)
