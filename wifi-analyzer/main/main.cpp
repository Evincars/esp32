#include <cstdio>

#include "app_settings.h"
#include "board.h"
#include "boot_mode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "netid.h"
#include "nvs_flash.h"
#include "repeater.h"
#include "sniffer.h"
#include "sysmon.h"
#include "time_sync.h"
#include "touch.h"
#include "ui/ui.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

#define DOUBLE_RST_WINDOW_MS 2500

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static ui_screen_id_t start_screen(boot_view_t view)
{
    switch (view) {
    case BOOT_VIEW_MONITOR: return UI_SCR_SYSTEM;
    case BOOT_VIEW_CLOCK: return UI_SCR_CLOCK;
    default: return UI_SCR_MENU;
    }
}

extern "C" void app_main(void)
{
    board_power_init();
    nvs_init();
    app_settings_load();
    /* may put the board straight back to sleep on a double RST press */
    boot_view_t view = boot_mode_init();

    board_backlight_init();
    board_battery_init();
    sysmon_init();
    time_sync_init();
    touch_init();
    bool calibrated = touch_calib_load();

    /* applies any stored spoofed MAC identity before the Wi-Fi driver starts */
    netid_init();

    lvgl_port_init();
    wifi_mgr_init();
    sniffer_init();
    repeater_init();

    if (lvgl_port_lock(portMAX_DELAY)) {
        ui_init(!calibrated, start_screen(view));
        lvgl_port_unlock();
    }

    /* Let the first frame land, then fade the backlight in. */
    vTaskDelay(pdMS_TO_TICKS(80));
    for (int b = 0; b <= g_settings.brightness; b += 4) {
        board_backlight_set(b);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    board_backlight_set(g_settings.brightness);

    if (g_settings.autoconnect && wifi_mgr_autoconnect()) {
        ESP_LOGI(TAG, "auto-connecting to the last saved network");
    }
    if (g_settings.web_autostart) web_server_start();

    /* a second RST press after this point is a normal single press again */
    int64_t elapsed_ms = esp_timer_get_time() / 1000;
    if (elapsed_ms < DOUBLE_RST_WINDOW_MS) vTaskDelay(pdMS_TO_TICKS(DOUBLE_RST_WINDOW_MS - elapsed_ms));
    boot_mode_disarm();
}
