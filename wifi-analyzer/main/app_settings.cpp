#include "app_settings.h"

#include "nvs.h"

#define NVS_NS "settings"
#define NVS_KEY "app"

app_settings_t g_settings = {
    .brightness = 80,
    .autoconnect = true,
    .scan_interval_s = 5,
    .palette = 0,
    .timezone = 2, /* Central Europe */
    .web_autostart = false,
    .rst_shortcuts = true,
};

void app_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    /* fields are only ever appended, so an older, shorter blob keeps the new defaults */
    app_settings_t s = g_settings;
    size_t len = sizeof(s);
    if (nvs_get_blob(h, NVS_KEY, &s, &len) == ESP_OK && len <= sizeof(s)) g_settings = s;
    nvs_close(h);
}

void app_settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, &g_settings, sizeof(g_settings));
    nvs_commit(h);
    nvs_close(h);
}
