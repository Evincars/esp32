#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

/* ---------- palette ---------- */
/* Every screen draws from one palette: neutrals plus three signal colors
   (accent = interactive / highlight, good = healthy / connected, bad = errors / weak). */
typedef struct {
    const char *name;
    uint32_t bg, header, surface, surface_hi, border, text, text_dim;
    uint32_t accent, good, bad;
} ui_palette_t;

#define UI_PALETTE_COUNT 3
extern const ui_palette_t UI_PALETTES[UI_PALETTE_COUNT];
extern const ui_palette_t *g_pal;

/* Switches the palette and the LVGL theme; screens built afterwards use it. */
void ui_theme_apply(int index);

#define UI_COL_BG lv_color_hex(g_pal->bg)
#define UI_COL_HEADER lv_color_hex(g_pal->header)
#define UI_COL_CARD lv_color_hex(g_pal->surface)
#define UI_COL_CARD_HI lv_color_hex(g_pal->surface_hi)
#define UI_COL_BORDER lv_color_hex(g_pal->border)
#define UI_COL_TEXT lv_color_hex(g_pal->text)
#define UI_COL_TEXT_DIM lv_color_hex(g_pal->text_dim)
#define UI_COL_ACCENT lv_color_hex(g_pal->accent)
#define UI_COL_GOOD lv_color_hex(g_pal->good)
#define UI_COL_BAD lv_color_hex(g_pal->bad)
/* Text on top of a filled accent/good/bad surface. */
#define UI_COL_ON_FILL lv_color_hex(g_pal->bg)

#define UI_RADIUS 4
#define UI_HEADER_H 26
#define UI_CONTENT_H (240 - UI_HEADER_H)

typedef enum {
    UI_SCR_MENU,
    UI_SCR_SCANNER,
    UI_SCR_CHANNELS,
    UI_SCR_NETWORK,
    UI_SCR_PORTS,
    UI_SCR_SNIFFER,
    UI_SCR_PACKET,
    UI_SCR_DEAUTH,
    UI_SCR_BEACON,
    UI_SCR_ROGUE,
    UI_SCR_CSI,
    UI_SCR_LOCATE,
    UI_SCR_REPEATER,
    UI_SCR_IDENTITY,
    UI_SCR_WEB,
    UI_SCR_SYSTEM,
    UI_SCR_CLOCK,
    UI_SCR_SETTINGS,
    UI_SCR_ABOUT,
    UI_SCR_CALIBRATE,
} ui_screen_id_t;

/* Entry point: shows the splash (normal boots), then calibration if needed, then `start`. */
void ui_init(bool need_calibration, ui_screen_id_t start);

/* ---------- navigation ---------- */
void ui_go(ui_screen_id_t id);
/* Back = return to the main menu (BOOT button does the same), unless the
   current screen redirected it with ui_set_back_target(). */
void ui_back(void);
void ui_set_back_target(ui_screen_id_t id);
/* Rebuilds the current screen in place (after a palette change). */
void ui_reload(void);
/* Loads an already-built screen with a slide animation. */
void ui_load(lv_obj_t *scr, bool forward);

/* ---------- building blocks (ui_common.cpp) ---------- */
/* Creates a screen with the shared header; returns the content area below it. */
lv_obj_t *ui_screen_create(const char *title, bool with_back);
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
/* Filled button in `color` (accent for primary actions, bad for destructive ones). */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color);
/* Outlined, neutral button for secondary actions. */
lv_obj_t *ui_button_outline(lv_obj_t *parent, const char *text);
/* Small upper-case caption above a group of rows. */
lv_obj_t *ui_caption(lv_obj_t *parent, const char *text);
/* "key   value" row inside a card; returns the value label. */
lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *key);
/* Thin progress bar in the accent color. */
lv_obj_t *ui_bar(lv_obj_t *parent, int32_t w, int32_t h);
/* Timer that is deleted together with the screen `owner` belongs to. */
lv_timer_t *ui_screen_timer(lv_obj_t *owner, lv_timer_cb_t cb, uint32_t period_ms, void *user);
void ui_toast(const char *text, lv_color_t color);
/* Short slide-up + fade-in entrance effect. */
void ui_anim_enter(lv_obj_t *obj, uint32_t delay_ms);
/* Signal-strength glyph: 4 bars filled by RSSI. */
lv_obj_t *ui_signal_bars(lv_obj_t *parent, int rssi);
void ui_signal_bars_set(lv_obj_t *bars, int rssi);
lv_color_t ui_rssi_color(int rssi);
void ui_fmt_mac(char *buf, size_t len, const uint8_t mac[6]);
/* Simple yes/no dialog; `on_yes` runs when the confirm button is pressed. */
void ui_confirm(const char *title, const char *text, const char *yes, lv_color_t yes_color, void (*on_yes)(void));

/* ---------- screens ---------- */
lv_obj_t *ui_menu_create(void);
lv_obj_t *ui_scanner_create(void);
lv_obj_t *ui_channels_create(void);
lv_obj_t *ui_network_create(void);
lv_obj_t *ui_ports_create(void);
lv_obj_t *ui_sniffer_create(void);
lv_obj_t *ui_packet_create(void);
lv_obj_t *ui_deauth_create(void);
lv_obj_t *ui_beacon_create(void);
lv_obj_t *ui_rogue_create(void);
lv_obj_t *ui_csi_create(void);
lv_obj_t *ui_locate_create(void);
lv_obj_t *ui_repeater_create(void);
lv_obj_t *ui_identity_create(void);
lv_obj_t *ui_web_create(void);
lv_obj_t *ui_system_create(void);
lv_obj_t *ui_clock_create(void);
lv_obj_t *ui_settings_create(void);
lv_obj_t *ui_about_create(void);
lv_obj_t *ui_calibrate_create(void);
void ui_splash_show(bool then_calibrate, ui_screen_id_t then);

/* Opens the port scanner pre-set to one host (ip in network byte order). */
void ui_ports_open(uint32_t ip);
/* Opens the dissection of one captured packet. */
void ui_packet_open(uint32_t seq);

/* ---------- text entry + connect flow (ui_keyboard.cpp) ---------- */
typedef void (*ui_text_done_cb_t)(const char *text, void *user);
/* Full-screen on-screen keyboard. `on_done` runs with the text on OK; Cancel/back returns to `back_to`. */
void ui_text_input(const char *title, const char *placeholder, const char *initial, bool password,
                   ui_text_done_cb_t on_done, void *user, ui_screen_id_t back_to);
/* Asks for a password when needed, then connects and switches to the Network screen. */
void ui_connect_flow(const char *ssid, bool needs_password);
