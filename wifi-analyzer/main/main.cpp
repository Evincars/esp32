#include <cstdio>

#include "app_settings.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "nvs_flash.h"
#include "touch.h"
#include "ui/ui.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

extern "C" void app_main(void)
{
    board_power_init();
    nvs_init();
    app_settings_load();

    board_backlight_init();
    board_battery_init();
    touch_init();
    bool calibrated = touch_calib_load();

    lvgl_port_init();
    wifi_mgr_init();

    if (lvgl_port_lock(portMAX_DELAY)) {
        ui_init(!calibrated);
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
}
