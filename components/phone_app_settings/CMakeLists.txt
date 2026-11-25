file(GLOB FONT_SRCS fonts/*.c)

idf_component_register(
    SRCS phone_app_settings.cpp phone_settings_icon.c phone_net.cpp phone_locale.cpp phone_power.cpp phone_audio.cpp phone_storage.cpp phone_lock.cpp phone_volume.cpp phone_monitor.cpp phone_memdump.cpp phone_speedtest.cpp phone_membench.cpp phone_sdbench.cpp phone_celltest.cpp phone_cell.cpp phone_cell_icons.c phone_icon_no_battery.c phone_quick.cpp phone_led.cpp
         phone_fonts.c phone_ui.c phone_icon_mute.c ${FONT_SRCS}
    INCLUDE_DIRS .
    REQUIRES brookesia_core esp32p4c5_bsp esp_app_format fatfs esp_driver_sdmmc spi_flash esp_psram esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_client mbedtls
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# 4G data counter (phone_cell.cpp): bytes of the PPP netif
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_netif_transmit" "-Wl,--wrap=esp_netif_receive")
