#include <cmath>
#include <cstdio>
#include <ctime>

#include "app_settings.h"
#include "board.h"
#include "boot_mode.h"
#include "sysmon.h"
#include "time_sync.h"
#include "ui.h"
#include "wifi_mgr.h"

/* ---------------- palettes ---------------- */

const ui_palette_t UI_PALETTES[UI_PALETTE_COUNT] = {
    {"Graphite", 0x0F1115, 0x0A0C0F, 0x171A20, 0x20242C, 0x2A2F38, 0xE6E8EC, 0x8A919C, 0x4C8DFF, 0x3CCB8B, 0xE5534B},
    {"Midnight", 0x0A1020, 0x070B16, 0x111A2E, 0x18233B, 0x24324F, 0xE2E8F4, 0x7E8BA5, 0x2BC4E0, 0x4ADE9A, 0xF06A6A},
    {"Ember", 0x14110F, 0x0E0C0A, 0x1D1916, 0x27221E, 0x3A322C, 0xEDE7E1, 0x9C9088, 0xF0A33A, 0x9CCB5A, 0xE5574A},
};
const ui_palette_t *g_pal = &UI_PALETTES[0];

void ui_theme_apply(int index)
{
    if (index < 0 || index >= UI_PALETTE_COUNT) index = 0;
    g_pal = &UI_PALETTES[index];
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, UI_COL_ACCENT, UI_COL_GOOD, true, &lv_font_montserrat_14);
    lv_display_set_theme(disp, th);
}

/* ---------------- navigation ---------------- */

static ui_screen_id_t s_current = UI_SCR_MENU;
static ui_screen_id_t s_back_target = UI_SCR_MENU;

/* Header widgets of the screen that is currently being shown. */
static lv_obj_t *s_hdr_screen;
static lv_obj_t *s_hdr_net;
static lv_obj_t *s_hdr_batt;
static lv_obj_t *s_toast;

static lv_obj_t *create_screen(ui_screen_id_t id)
{
    switch (id) {
    case UI_SCR_SCANNER: return ui_scanner_create();
    case UI_SCR_CHANNELS: return ui_channels_create();
    case UI_SCR_NETWORK: return ui_network_create();
    case UI_SCR_PORTS: return ui_ports_create();
    case UI_SCR_SNIFFER: return ui_sniffer_create();
    case UI_SCR_PACKET: return ui_packet_create();
    case UI_SCR_DEAUTH: return ui_deauth_create();
    case UI_SCR_BEACON: return ui_beacon_create();
    case UI_SCR_ROGUE: return ui_rogue_create();
    case UI_SCR_CSI: return ui_csi_create();
    case UI_SCR_LOCATE: return ui_locate_create();
    case UI_SCR_REPEATER: return ui_repeater_create();
    case UI_SCR_IDENTITY: return ui_identity_create();
    case UI_SCR_BLUETOOTH: return ui_bluetooth_create();
    case UI_SCR_WEB: return ui_web_create();
    case UI_SCR_SYSTEM: return ui_system_create();
    case UI_SCR_CLOCK: return ui_clock_create();
    case UI_SCR_SETTINGS: return ui_settings_create();
    case UI_SCR_ABOUT: return ui_about_create();
    case UI_SCR_CALIBRATE: return ui_calibrate_create();
    default: return ui_menu_create();
    }
}

static void set_current(ui_screen_id_t id)
{
    s_current = id;
    /* lets the next RST press continue from the view that is on screen */
    boot_mode_note_view(id == UI_SCR_SYSTEM ? BOOT_VIEW_MONITOR : id == UI_SCR_CLOCK ? BOOT_VIEW_CLOCK : BOOT_VIEW_MENU);
}

void ui_load(lv_obj_t *scr, bool forward)
{
    lv_screen_load_anim(scr, forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, true);
}

void ui_go(ui_screen_id_t id)
{
    s_back_target = UI_SCR_MENU; /* screens without the shared header don't reset it themselves */
    set_current(id);
    lv_obj_t *scr = create_screen(id);
    if (id == UI_SCR_CALIBRATE || id == UI_SCR_CLOCK) {
        lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, true);
        return;
    }
    ui_load(scr, id != UI_SCR_MENU);
}

void ui_back(void)
{
    ui_screen_id_t target = s_back_target;
    s_back_target = UI_SCR_MENU;
    set_current(target);
    ui_load(create_screen(target), false);
}

void ui_set_back_target(ui_screen_id_t id)
{
    s_back_target = id;
}

void ui_reload(void)
{
    ui_screen_id_t back = s_back_target;
    lv_obj_t *scr = create_screen(s_current);
    s_back_target = back;
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

/* ---------------- header status ---------------- */

static void update_status(lv_timer_t *)
{
    static uint32_t tick;
    if (!s_hdr_net) return;
    tick++;

    wifi_conn_info_t info;
    switch (wifi_mgr_state()) {
    case WIFI_ST_CONNECTED:
        if (wifi_mgr_conn_info(&info)) {
            /* name + a rough distance estimate from RSSI (log-distance path loss,
               tuned for this board indoors so a typical connected signal reads a few
               metres); scrolls if long, tinted by signal quality */
            float d = powf(10.0f, (-50 - info.rssi) / 38.0f);
            char dist[12];
            if (d < 1.0f) snprintf(dist, sizeof(dist), "<1m");
            else if (d < 10.0f) snprintf(dist, sizeof(dist), "~%.1fm", d);
            else snprintf(dist, sizeof(dist), "~%dm", (int)(d + 0.5f));
            lv_label_set_text_fmt(s_hdr_net, "%s  %s", info.ssid[0] ? info.ssid : "connected", dist);
            lv_obj_set_style_text_color(s_hdr_net, ui_rssi_color(info.rssi), 0);
        }
        lv_obj_set_style_text_opa(s_hdr_net, LV_OPA_COVER, 0);
        break;
    case WIFI_ST_CONNECTING:
        lv_label_set_text(s_hdr_net, "joining");
        lv_obj_set_style_text_color(s_hdr_net, UI_COL_ACCENT, 0);
        lv_obj_set_style_text_opa(s_hdr_net, (tick & 1) ? LV_OPA_40 : LV_OPA_COVER, 0);
        break;
    case WIFI_ST_FAILED:
        lv_label_set_text(s_hdr_net, "failed");
        lv_obj_set_style_text_color(s_hdr_net, UI_COL_BAD, 0);
        lv_obj_set_style_text_opa(s_hdr_net, LV_OPA_COVER, 0);
        break;
    default:
        lv_label_set_text(s_hdr_net, "offline");
        lv_obj_set_style_text_color(s_hdr_net, UI_COL_TEXT_DIM, 0);
        lv_obj_set_style_text_opa(s_hdr_net, LV_OPA_COVER, 0);
        break;
    }

    sysmon_t s;
    sysmon_get(&s);
    if (s.on_usb) {
        lv_label_set_text(s_hdr_batt, "USB");
        lv_obj_set_style_text_color(s_hdr_batt, UI_COL_TEXT_DIM, 0);
    } else {
        lv_label_set_text_fmt(s_hdr_batt, "%d%%", s.batt_pct);
        lv_obj_set_style_text_color(s_hdr_batt, s.batt_pct <= 10 ? UI_COL_BAD : UI_COL_TEXT_DIM, 0);
    }
    /* the clock lives on its own screen, so the top bar spends its width on the
       breadcrumb (screen title) + network name + distance instead */
}

static void on_screen_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_hdr_screen) {
        s_hdr_screen = nullptr;
        s_hdr_net = nullptr;
        s_hdr_batt = nullptr;
    }
}

static void on_back_clicked(lv_event_t *)
{
    ui_back();
}

lv_obj_t *ui_screen_create(const char *title, bool with_back)
{
    s_back_target = UI_SCR_MENU;
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, nullptr);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), UI_HEADER_H);
    lv_obj_set_style_bg_color(hdr, UI_COL_HEADER, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hdr, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    /* a single flex row: [back] title  <SSID+distance grows>  time  batt */
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(hdr, with_back ? 0 : 10, 0);
    lv_obj_set_style_pad_right(hdr, 8, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);

    if (with_back) {
        lv_obj_t *back = lv_button_create(hdr);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, 34, UI_HEADER_H - 1);
        lv_obj_set_style_bg_color(back, UI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *arrow = ui_label(back, &lv_font_montserrat_14, UI_COL_TEXT_DIM, LV_SYMBOL_LEFT);
        lv_obj_center(arrow);
    }

    /* breadcrumb: the screen title takes the leftover width and is the last thing to
       ellipsize, so it stays readable ("where am I") no matter what else is shown */
    lv_obj_t *t = ui_label(hdr, &lv_font_montserrat_14, UI_COL_TEXT, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_width(t, 1);
    lv_obj_set_style_min_width(t, 48, 0);

    /* network name + distance: capped so it can't crowd out the breadcrumb, scrolls if long */
    s_hdr_net = ui_label(hdr, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(s_hdr_net, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_max_width(s_hdr_net, 150, 0);
    lv_obj_set_style_text_align(s_hdr_net, LV_TEXT_ALIGN_RIGHT, 0);

    s_hdr_batt = ui_label(hdr, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
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
    lv_obj_set_style_radius(c, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
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
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_lighten(color, LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_14, UI_COL_ON_FILL, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_button_outline(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, UI_COL_CARD_HI, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_14, UI_COL_TEXT, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = ui_label(parent, &lv_font_montserrat_10, UI_COL_TEXT_DIM, text);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *key)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT_DIM, key);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, "-");
    lv_obj_set_width(v, LV_PCT(62));
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

lv_obj_t *ui_bar(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 400, 0);
    return bar;
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

/* ---------------- dialogs ---------------- */

static void on_confirm_yes(lv_event_t *e)
{
    lv_obj_t *mb = (lv_obj_t *)lv_event_get_user_data(e);
    auto cb = (void (*)(void))lv_obj_get_user_data(mb);
    lv_msgbox_close(mb);
    if (cb) cb();
}

static void on_confirm_no(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
}

void ui_confirm(const char *title, const char *text, const char *yes, lv_color_t yes_color, void (*on_yes)(void))
{
    lv_obj_t *mb = lv_msgbox_create(nullptr);
    lv_obj_set_width(mb, 260);
    lv_obj_set_user_data(mb, (void *)on_yes);
    lv_obj_set_style_bg_color(mb, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(mb, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(mb, 1, 0);
    lv_obj_set_style_radius(mb, UI_RADIUS, 0);
    lv_msgbox_add_title(mb, title);
    lv_msgbox_add_text(mb, text);
    lv_obj_t *no = lv_msgbox_add_footer_button(mb, "Cancel");
    lv_obj_set_style_radius(no, 3, 0);
    lv_obj_set_style_bg_color(no, UI_COL_CARD_HI, 0);
    lv_obj_add_event_cb(no, on_confirm_no, LV_EVENT_CLICKED, mb);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, yes);
    lv_obj_set_style_radius(ok, 3, 0);
    lv_obj_set_style_bg_color(ok, yes_color, 0);
    lv_obj_set_style_text_color(ok, UI_COL_ON_FILL, 0);
    lv_obj_add_event_cb(ok, on_confirm_yes, LV_EVENT_CLICKED, mb);
}

/* ---------------- effects ---------------- */

static void enter_exec(void *obj, int32_t v)
{
    /* v: 0 -> 256 */
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)(v >= 256 ? 255 : v), 0);
    lv_obj_set_style_translate_y((lv_obj_t *)obj, 8 - v * 8 / 256, 0);
}

void ui_anim_enter(lv_obj_t *obj, uint32_t delay_ms)
{
    enter_exec(obj, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, enter_exec);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, 240);
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
    lv_obj_set_style_radius(t, UI_RADIUS, 0);
    lv_obj_set_style_border_color(t, color, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(t, 3, 0);
    lv_obj_set_style_pad_hor(t, 12, 0);
    lv_obj_set_style_pad_ver(t, 7, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(t, &lv_font_montserrat_14, UI_COL_TEXT, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 272, 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -10);
    ui_anim_enter(t, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, toast_opa_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_delay(&a, 2200);
    lv_anim_set_completed_cb(&a, toast_out_ready);
    lv_anim_start(&a);
}

/* ---------------- signal glyph ---------------- */

lv_color_t ui_rssi_color(int rssi)
{
    if (rssi >= -60) return UI_COL_GOOD;
    if (rssi >= -75) return UI_COL_ACCENT;
    return UI_COL_BAD;
}

lv_obj_t *ui_signal_bars(lv_obj_t *parent, int rssi)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 15, 13);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 3, 4 + i * 3);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 4, 0);
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

/* The physical BOOT and USER (power) buttons. BOOT cycles the main views on
   top-level screens and backs out of sub-screens; USER hibernates the display. */
#define BOOT_LONG_TICKS 30 /* 30 * 50 ms = 1.5 s hold to power off */

static void boot_button_poll(lv_timer_t *)
{
    static bool boot_was_down, user_was_down;
    static int boot_ticks;
    static bool boot_long_fired, boot_woke;

    bool user_down = board_user_button_pressed();
    if (user_down && !user_was_down) {
        if (board_display_asleep()) board_display_wake(g_settings.brightness);
        else board_display_sleep();
    }
    user_was_down = user_down;

    bool boot_down = board_boot_button_pressed();
    if (boot_down && !boot_was_down) {
        /* press: start timing the hold; a press that wakes the display does nothing else */
        boot_ticks = 0;
        boot_long_fired = false;
        boot_woke = board_display_asleep();
        if (boot_woke) board_display_wake(g_settings.brightness);
    } else if (boot_down) {
        /* long hold -> deep sleep (the RST button only resets, so BOOT powers off) */
        if (!boot_long_fired && !boot_woke && ++boot_ticks >= BOOT_LONG_TICKS && !board_display_asleep()) {
            boot_long_fired = true;
            boot_mode_sleep();
        }
    } else if (boot_was_down && !boot_long_fired && !boot_woke) {
        /* short press on release: cycle the top-level views or back out of a sub-screen */
        if (s_current == UI_SCR_MENU) ui_go(UI_SCR_CLOCK);
        else if (s_current == UI_SCR_CLOCK) ui_go(UI_SCR_SYSTEM);
        else if (s_current == UI_SCR_SYSTEM) ui_go(UI_SCR_MENU);
        else if (s_current != UI_SCR_CALIBRATE) ui_back();
    }
    boot_was_down = boot_down;
}

void ui_init(bool need_calibration, ui_screen_id_t start)
{
    ui_theme_apply(g_settings.palette);
    lv_timer_create(update_status, 1000, nullptr);
    lv_timer_create(boot_button_poll, 50, nullptr);

    if (start != UI_SCR_MENU && !need_calibration) {
        /* RST shortcut: skip the splash so the view shows up right away */
        set_current(start);
        lv_screen_load(create_screen(start));
        return;
    }
    ui_splash_show(need_calibration, start);
}
