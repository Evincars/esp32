idf_component_register(
    SRCS
        "main.cpp"
        "app_settings.cpp"
        "board.cpp"
        "lvgl_port.cpp"
        "touch.cpp"
        "wifi_mgr.cpp"
        "ui/ui_about.cpp"
        "ui/ui_calibrate.cpp"
        "ui/ui_channels.cpp"
        "ui/ui_common.cpp"
        "ui/ui_keyboard.cpp"
        "ui/ui_menu.cpp"
        "ui/ui_network.cpp"
        "ui/ui_scanner.cpp"
        "ui/ui_settings.cpp"
        "ui/ui_splash.cpp"
        "ui/ui_system.cpp"
    INCLUDE_DIRS "." "ui"
    PRIV_REQUIRES
        esp_adc
        esp_app_format
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_driver_tsens
        esp_event
        esp_lcd
        esp_netif
        esp_timer
        esp_wifi
        lwip
        nvs_flash
        spi_flash
)

# LVGL style selectors OR different enums together (LV_PART_* | LV_STATE_*), which C++20 flags.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-enum-enum-conversion)
