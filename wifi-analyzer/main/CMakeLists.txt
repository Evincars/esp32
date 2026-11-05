idf_component_register(
    SRCS
        "main.cpp"
        "app_settings.cpp"
        "board.cpp"
        "boot_mode.cpp"
        "lvgl_port.cpp"
        "netid.cpp"
        "oui.cpp"
        "pkt_decode.cpp"
        "portscan.cpp"
        "repeater.cpp"
        "sniffer.cpp"
        "sysmon.cpp"
        "time_sync.cpp"
        "touch.cpp"
        "web_server.cpp"
        "wifi_mgr.cpp"
        "wifi_attack.cpp"
        "rogue_ap.cpp"
        "wifi_csi.cpp"
        "wifi_ftm.cpp"
        "ui/ui_about.cpp"
        "ui/ui_calibrate.cpp"
        "ui/ui_channels.cpp"
        "ui/ui_clock.cpp"
        "ui/ui_common.cpp"
        "ui/ui_keyboard.cpp"
        "ui/ui_menu.cpp"
        "ui/ui_network.cpp"
        "ui/ui_ports.cpp"
        "ui/ui_scanner.cpp"
        "ui/ui_settings.cpp"
        "ui/ui_sniffer.cpp"
        "ui/ui_splash.cpp"
        "ui/ui_system.cpp"
        "ui/ui_web.cpp"
        "ui/ui_deauth.cpp"
        "ui/ui_beacon.cpp"
        "ui/ui_rogue.cpp"
        "ui/ui_csi.cpp"
        "ui/ui_locate.cpp"
        "ui/ui_repeater.cpp"
    INCLUDE_DIRS "." "ui"
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES
        app_update
        bootloader_support
        esp_adc
        esp_app_format
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_driver_tsens
        esp_event
        esp_http_server
        esp_lcd
        esp_netif
        esp_partition
        esp_timer
        esp_wifi
        espressif__esp-dsp
        lwip
        nvs_flash
        spi_flash
)

# LVGL style selectors OR different enums together (LV_PART_* | LV_STATE_*), which C++20 flags.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-enum-enum-conversion)

# wifi_attack.cpp overrides the Wi-Fi driver's raw-frame sanity hook so deauth frames
# can be transmitted; the symbol also exists (non-weak) in the closed-source Wi-Fi blob,
# so let the linker keep our definition instead of erroring on the duplicate.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
