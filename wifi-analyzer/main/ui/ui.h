#pragma once

#include "lvgl.h"

/* ---------- palette ---------- */
#define UI_COL_BG_TOP lv_color_hex(0x070B16)
#define UI_COL_BG_BOTTOM lv_color_hex(0x111A33)
#define UI_COL_HEADER lv_color_hex(0x0A1020)
#define UI_COL_CARD lv_color_hex(0x141C33)
#define UI_COL_CARD_HI lv_color_hex(0x1C2747)
#define UI_COL_BORDER lv_color_hex(0x24325C)
#define UI_COL_TEXT lv_color_hex(0xE8EEFF)
#define UI_COL_TEXT_DIM lv_color_hex(0x8A97B8)
#define UI_COL_CYAN lv_color_hex(0x00E0FF)
#define UI_COL_PURPLE lv_color_hex(0x9B6CFF)
#define UI_COL_GREEN lv_color_hex(0x22E39A)
#define UI_COL_AMBER lv_color_hex(0xFFB020)
#define UI_COL_BLUE lv_color_hex(0x5B8CFF)
#define UI_COL_MAGENTA lv_color_hex(0xFF3D9A)
#define UI_COL_RED lv_color_hex(0xFF4D5E)

#define UI_HEADER_H 30
#define UI_CONTENT_H (240 - UI_HEADER_H)

typedef enum {
    UI_SCR_MENU,
    UI_SCR_SCANNER,
    UI_SCR_CHANNELS,
    UI_SCR_NETWORK,
    UI_SCR_SYSTEM,
    UI_SCR_SETTINGS,
    UI_SCR_ABOUT,
    UI_SCR_CALIBRATE,
} ui_screen_id_t;

/* Entry point: shows the splash, then calibration (if needed) or the menu. */
void ui_init(bool need_calibration);

/* ---------- navigation ---------- */
void ui_go(ui_screen_id_t id);
/* Back = return to the main menu (BOOT button does the same), unless the
   current screen redirected it with ui_set_back_target(). */
void ui_back(void);
void ui_set_back_target(ui_screen_id_t id);
/* Loads an already-built screen with a slide animation. */
void ui_load(lv_obj_t *scr, bool forward);

/* ---------- building blocks (ui_common.cpp) ---------- */
/* Creates a screen with the shared header; returns the content area below it. */
lv_obj_t *ui_screen_create(const char *title, lv_color_t accent, bool with_back);
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color);
/* "key   value" row inside a card; returns the value label. */
lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *key);
/* Timer that is deleted together with the screen `owner` belongs to. */
lv_timer_t *ui_screen_timer(lv_obj_t *owner, lv_timer_cb_t cb, uint32_t period_ms, void *user);
void ui_toast(const char *text, lv_color_t color);
/* Slide-up + fade-in entrance effect. */
void ui_anim_enter(lv_obj_t *obj, uint32_t delay_ms);
/* Signal-strength glyph: 4 bars filled by RSSI. */
lv_obj_t *ui_signal_bars(lv_obj_t *parent, int rssi);
void ui_signal_bars_set(lv_obj_t *bars, int rssi);
lv_color_t ui_rssi_color(int rssi);
void ui_fmt_mac(char *buf, size_t len, const uint8_t mac[6]);

/* ---------- screens ---------- */
lv_obj_t *ui_menu_create(void);
lv_obj_t *ui_scanner_create(void);
lv_obj_t *ui_channels_create(void);
lv_obj_t *ui_network_create(void);
lv_obj_t *ui_system_create(void);
lv_obj_t *ui_settings_create(void);
lv_obj_t *ui_about_create(void);
lv_obj_t *ui_calibrate_create(void);
void ui_splash_show(bool then_calibrate);

/* ---------- text entry + connect flow (ui_keyboard.cpp) ---------- */
typedef void (*ui_text_done_cb_t)(const char *text, void *user);
/* Full-screen on-screen keyboard. `on_done` runs with the text on OK; Cancel/back returns to `back_to`. */
void ui_text_input(const char *title, const char *placeholder, const char *initial, bool password,
                   ui_text_done_cb_t on_done, void *user, ui_screen_id_t back_to);
/* Asks for a password when needed, then connects and switches to the Network screen. */
void ui_connect_flow(const char *ssid, bool needs_password);
