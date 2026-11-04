#include <cstdio>

#include "board.h"
#include "ui.h"
#include "wifi_mgr.h"

static ui_screen_id_t s_current = UI_SCR_MENU;
static ui_screen_id_t s_back_target = UI_SCR_MENU;

/* Header widgets of the screen that is currently being shown. */
static lv_obj_t *s_hdr_screen;
static lv_obj_t *s_hdr_wifi;
static lv_obj_t *s_hdr_batt;
static lv_obj_t *s_toast;

/* ---------------- navigation ---------------- */

void ui_load(lv_obj_t *scr, bool forward)
{
    lv_screen_load_anim(scr, forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT, 260, 0, true);
}

void ui_go(ui_screen_id_t id)
{
    lv_obj_t *scr = nullptr;
    switch (id) {
    case UI_SCR_MENU: scr = ui_menu_create(); break;
    case UI_SCR_SCANNER: scr = ui_scanner_create(); break;
    case UI_SCR_CHANNELS: scr = ui_channels_create(); break;
    case UI_SCR_NETWORK: scr = ui_network_create(); break;
    case UI_SCR_SYSTEM: scr = ui_system_create(); break;
    case UI_SCR_SETTINGS: scr = ui_settings_create(); break;
    case UI_SCR_ABOUT: scr = ui_about_create(); break;
    case UI_SCR_CALIBRATE:
        scr = ui_calibrate_create();
        s_current = id;
        lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
        return;
    }
    s_current = id;
    ui_load(scr, id != UI_SCR_MENU);
}

void ui_back(void)
{
    ui_screen_id_t target = s_back_target;
    s_back_target = UI_SCR_MENU;
    if (target == UI_SCR_MENU) {
        ui_go(UI_SCR_MENU);
        return;
    }
    /* going "back" to a sub-screen still slides to the right */
    s_current = target;
    lv_obj_t *scr = nullptr;
    switch (target) {
    case UI_SCR_SCANNER: scr = ui_scanner_create(); break;
    case UI_SCR_NETWORK: scr = ui_network_create(); break;
    default: scr = ui_menu_create(); s_current = UI_SCR_MENU; break;
    }
    ui_load(scr, false);
}

void ui_set_back_target(ui_screen_id_t id)
{
    s_back_target = id;
}

/* ---------------- header status ---------------- */

static void update_status(lv_timer_t *)
{
    static uint32_t tick;
    static int batt_mv;
    if (!s_hdr_wifi) return;

    switch (wifi_mgr_state()) {
    case WIFI_ST_CONNECTED:
        lv_obj_set_style_text_color(s_hdr_wifi, UI_COL_GREEN, 0);
        lv_obj_set_style_text_opa(s_hdr_wifi, LV_OPA_COVER, 0);
        break;
    case WIFI_ST_CONNECTING:
        lv_obj_set_style_text_color(s_hdr_wifi, UI_COL_AMBER, 0);
        lv_obj_set_style_text_opa(s_hdr_wifi, (tick & 1) ? LV_OPA_30 : LV_OPA_COVER, 0);
        break;
    case WIFI_ST_FAILED:
        lv_obj_set_style_text_color(s_hdr_wifi, UI_COL_RED, 0);
        lv_obj_set_style_text_opa(s_hdr_wifi, LV_OPA_COVER, 0);
        break;
    default:
        lv_obj_set_style_text_color(s_hdr_wifi, UI_COL_TEXT_DIM, 0);
        lv_obj_set_style_text_opa(s_hdr_wifi, LV_OPA_50, 0);
        break;
    }

    if (tick % 10 == 0 || batt_mv == 0) batt_mv = board_battery_mv();
    tick++;

    const char *sym;
    lv_color_t col = UI_COL_TEXT_DIM;
    if (batt_mv > 4300 || batt_mv < 2500) {
        sym = LV_SYMBOL_USB; /* no battery fitted, running from USB */
    } else {
        int pct = board_battery_percent(batt_mv);
        sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 55 ? LV_SYMBOL_BATTERY_3 : pct > 30 ? LV_SYMBOL_BATTERY_2
              : pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        if (pct <= 10) col = UI_COL_RED;
    }
    lv_label_set_text(s_hdr_batt, sym);
    lv_obj_set_style_text_color(s_hdr_batt, col, 0);
}

static void on_screen_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_hdr_screen) {
        s_hdr_screen = nullptr;
        s_hdr_wifi = nullptr;
        s_hdr_batt = nullptr;
    }
}

static void on_back_clicked(lv_event_t *)
{
    ui_back();
}

lv_obj_t *ui_screen_create(const char *title, lv_color_t accent, bool with_back)
{
    s_back_target = UI_SCR_MENU;
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COL_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, UI_COL_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, nullptr);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), UI_HEADER_H);
    lv_obj_set_style_bg_color(hdr, UI_COL_HEADER, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* glowing accent line under the header, fading out to the right */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), 2);
    lv_obj_set_pos(line, 0, UI_HEADER_H - 2);
    lv_obj_set_style_bg_color(line, accent, 0);
    lv_obj_set_style_bg_grad_color(line, accent, 0);
    lv_obj_set_style_bg_grad_dir(line, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_main_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    int title_x = 10;
    if (with_back) {
        lv_obj_t *back = lv_button_create(hdr);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, 44, UI_HEADER_H);
        lv_obj_set_style_bg_color(back, accent, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *arrow = ui_label(back, &lv_font_montserrat_16, accent, LV_SYMBOL_LEFT);
        lv_obj_center(arrow);
        title_x = 44;
    }

    lv_obj_t *t = ui_label(hdr, &lv_font_montserrat_16, UI_COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, title_x, -1);

    s_hdr_batt = ui_label(hdr, &lv_font_montserrat_14, UI_COL_TEXT_DIM, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(s_hdr_batt, LV_ALIGN_RIGHT_MID, -8, -1);
    s_hdr_wifi = ui_label(hdr, &lv_font_montserrat_14, UI_COL_TEXT_DIM, LV_SYMBOL_WIFI);
    lv_obj_align(s_hdr_wifi, LV_ALIGN_RIGHT_MID, -36, -1);
    s_hdr_screen = scr;
    update_status(nullptr);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, UI_HEADER_H);
    lv_obj_set_size(content, LV_PCT(100), UI_CONTENT_H);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);
    return content;
}

/* ---------------- widgets ---------------- */

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_color(c, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_color(b, lv_color_lighten(color, LV_OPA_30), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_color(b, color, 0);
    lv_obj_set_style_shadow_width(b, 12, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_shadow_spread(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 7, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_transform_width(b, -2, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(b, -2, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_14, lv_color_hex(0x071022), text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *key)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT_DIM, key);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, "-");
    lv_obj_set_width(v, LV_PCT(64));
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

static void on_owner_delete(lv_event_t *e)
{
    lv_timer_delete((lv_timer_t *)lv_event_get_user_data(e));
}

lv_timer_t *ui_screen_timer(lv_obj_t *owner, lv_timer_cb_t cb, uint32_t period_ms, void *user)
{
    lv_timer_t *t = lv_timer_create(cb, period_ms, user);
    lv_obj_add_event_cb(owner, on_owner_delete, LV_EVENT_DELETE, t);
    return t;
}

/* ---------------- effects ---------------- */

static void enter_exec(void *obj, int32_t v)
{
    /* v: 0 -> 256 */
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)(v >= 256 ? 255 : v), 0);
    lv_obj_set_style_translate_y((lv_obj_t *)obj, 18 - v * 18 / 256, 0);
}

void ui_anim_enter(lv_obj_t *obj, uint32_t delay_ms)
{
    enter_exec(obj, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, enter_exec);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, 380);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void toast_out_ready(lv_anim_t *a)
{
    lv_obj_t *t = (lv_obj_t *)a->var;
    if (t == s_toast) s_toast = nullptr;
    lv_obj_delete(t);
}

static void toast_opa_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void toast_y_exec(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

void ui_toast(const char *text, lv_color_t color)
{
    if (s_toast) {
        lv_anim_delete(s_toast, nullptr);
        lv_obj_delete(s_toast);
    }
    lv_obj_t *t = lv_obj_create(lv_layer_top());
    s_toast = t;
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(t, 300, 0);
    lv_obj_set_style_bg_color(t, UI_COL_CARD_HI, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_border_color(t, color, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_shadow_color(t, color, 0);
    lv_obj_set_style_shadow_width(t, 18, 0);
    lv_obj_set_style_shadow_opa(t, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(t, 14, 0);
    lv_obj_set_style_pad_ver(t, 7, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(t, &lv_font_montserrat_14, UI_COL_TEXT, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 272, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, toast_y_exec);
    lv_anim_set_values(&a, 60, 0);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, toast_opa_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_delay(&a, 2200);
    lv_anim_set_completed_cb(&a, toast_out_ready);
    lv_anim_start(&a);
}

/* ---------------- signal glyph ---------------- */

lv_color_t ui_rssi_color(int rssi)
{
    if (rssi >= -55) return UI_COL_GREEN;
    if (rssi >= -67) return UI_COL_CYAN;
    if (rssi >= -78) return UI_COL_AMBER;
    return UI_COL_RED;
}

lv_obj_t *ui_signal_bars(lv_obj_t *parent, int rssi)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 19, 16);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 3, 4 + i * 4);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 5, 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    ui_signal_bars_set(box, rssi);
    return box;
}

void ui_signal_bars_set(lv_obj_t *bars, int rssi)
{
    int level = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : rssi >= -88 ? 1 : 0;
    lv_color_t on = ui_rssi_color(rssi);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(lv_obj_get_child(bars, i), i < level ? on : UI_COL_BORDER, 0);
    }
}

void ui_fmt_mac(char *buf, size_t len, const uint8_t mac[6])
{
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------------- boot / init ---------------- */

static void boot_button_poll(lv_timer_t *)
{
    static bool was_down;
    bool down = board_boot_button_pressed();
    if (down && !was_down && s_current != UI_SCR_MENU && s_current != UI_SCR_CALIBRATE) {
        ui_back();
    }
    was_down = down;
}

void ui_init(bool need_calibration)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, UI_COL_CYAN, UI_COL_MAGENTA, true, &lv_font_montserrat_14);
    lv_display_set_theme(disp, th);

    lv_timer_create(update_status, 1000, nullptr);
    lv_timer_create(boot_button_poll, 50, nullptr);

    ui_splash_show(need_calibration);
}
