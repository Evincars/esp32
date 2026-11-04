#pragma once

#include <cstdint>

typedef struct {
    uint8_t brightness;      /* 5..100 % */
    bool autoconnect;        /* reconnect to the last network at boot */
    uint8_t scan_interval_s; /* auto-refresh period on the scanner/channel screens */
    uint8_t palette;         /* index into UI_PALETTES */
    uint8_t timezone;        /* index into the time_sync zone table */
    bool web_autostart;      /* start the HTTP dashboard at boot */
    bool rst_shortcuts;      /* RST cycles the boot view, a double press puts the board to sleep */
} app_settings_t;

/* Global, loaded once at boot. Call app_settings_save() after changing it. */
extern app_settings_t g_settings;

void app_settings_load(void);
void app_settings_save(void);
