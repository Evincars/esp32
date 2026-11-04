#include "boot_mode.h"

#include "app_settings.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "boot_mode";

#define NVS_NS "boot"
#define KEY_ARMED "armed" /* set during the first seconds after a boot */
#define KEY_SLEPT "slept" /* we went to sleep; the next boot is a wake-up */
#define KEY_VIEW "view"   /* what was on screen last */

static boot_view_t s_view = BOOT_VIEW_MENU;
static bool s_armed;

static uint8_t nvs_read(const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = def;
    nvs_close(h);
    return v;
}

static void nvs_write(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

boot_view_t boot_mode_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool armed = nvs_read(KEY_ARMED, 0);
    bool slept = nvs_read(KEY_SLEPT, 0);
    uint8_t last = nvs_read(KEY_VIEW, BOOT_VIEW_MENU);
    if (last >= BOOT_VIEW_COUNT) last = BOOT_VIEW_MENU;

    boot_view_t view = BOOT_VIEW_MENU;
    /* RST (EN pin) shows up as a power-on reset; flashing over USB, panics and
       watchdog resets have their own reasons and always start at the menu. */
    if (reason == ESP_RST_POWERON && g_settings.rst_shortcuts) {
        if (armed) {
            ESP_LOGI(TAG, "RST pressed twice, going to sleep");
            boot_mode_sleep();
        }
        if (!slept) view = (boot_view_t)((last + 1) % BOOT_VIEW_COUNT);
        nvs_write(KEY_ARMED, 1);
        s_armed = true;
    } else if (armed) {
        nvs_write(KEY_ARMED, 0);
    }
    if (slept) nvs_write(KEY_SLEPT, 0);

    s_view = (boot_view_t)last;
    boot_mode_note_view(view);
    ESP_LOGI(TAG, "reset reason %d, start view %d", reason, view);
    return view;
}

void boot_mode_disarm(void)
{
    if (!s_armed) return;
    s_armed = false;
    nvs_write(KEY_ARMED, 0);
}

void boot_mode_note_view(boot_view_t view)
{
    if (view == s_view) return;
    s_view = view;
    nvs_write(KEY_VIEW, view);
}

void boot_mode_sleep(void)
{
    nvs_write(KEY_ARMED, 0);
    nvs_write(KEY_SLEPT, 1);
    nvs_write(KEY_VIEW, BOOT_VIEW_MENU);
    esp_wifi_stop(); /* harmless if Wi-Fi was never started */
    board_deep_sleep();
}
