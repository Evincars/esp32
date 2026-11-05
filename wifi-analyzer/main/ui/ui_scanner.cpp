#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app_settings.h"
#include "esp_timer.h"
#include "ui.h"
#include "wifi_mgr.h"

#define ROW_H 42
#define TOOLBAR_H 28
#define SHEET_H 204

typedef enum { SORT_SIGNAL, SORT_CHANNEL, SORT_NAME, SORT_COUNT } sort_mode_t;
static const char *SORT_NAMES[] = {"Signal", "Channel", "Name"};

static sort_mode_t s_sort = SORT_SIGNAL;

static struct {
    lv_obj_t *count_lbl;
    lv_obj_t *sort_btn_lbl;
    lv_obj_t *scan_btn;
    lv_obj_t *spinner;
    lv_obj_t *list;
    lv_obj_t *empty;
    wifi_ap_record_t aps[WIFI_MAX_APS];
    int ap_count;
    uint32_t seen_gen;
    int64_t last_scan_us;
    bool first_build;
} S;

static wifi_ap_record_t s_sel; /* AP shown in the detail sheet */

static void sort_aps(void)
{
    std::sort(S.aps, S.aps + S.ap_count, [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) {
        switch (s_sort) {
        case SORT_CHANNEL:
            if (a.primary != b.primary) return a.primary < b.primary;
            return a.rssi > b.rssi;
        case SORT_NAME: {
            int c = strcasecmp((const char *)a.ssid, (const char *)b.ssid);
            if (c != 0) return a.ssid[0] && (!b.ssid[0] || c < 0); /* hidden SSIDs last */
            return a.rssi > b.rssi;
        }
        default:
            return a.rssi > b.rssi;
        }
    });
}

/* ---------------- detail sheet ---------------- */

static void sheet_close(lv_obj_t *backdrop)
{
    lv_obj_delete_async(backdrop);
}

static void on_backdrop_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) sheet_close(lv_event_get_current_target_obj(e));
}

static void on_sheet_close(lv_event_t *e)
{
    sheet_close((lv_obj_t *)lv_event_get_user_data(e));
}

static void on_sheet_connect(lv_event_t *e)
{
    sheet_close((lv_obj_t *)lv_event_get_user_data(e));
    ui_connect_flow((const char *)s_sel.ssid, s_sel.authmode != WIFI_AUTH_OPEN);
}

static void sheet_y_exec(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void backdrop_opa_exec(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void open_sheet(const wifi_ap_record_t *ap)
{
    s_sel = *ap;
    char buf[64];

    lv_obj_t *backdrop = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(backdrop);
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, on_backdrop_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *sheet = lv_obj_create(backdrop);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, LV_PCT(100), SHEET_H);
    lv_obj_set_style_bg_color(sheet, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sheet, UI_RADIUS, 0);
    lv_obj_set_style_border_color(sheet, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_border_side(sheet, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(sheet, 8, 0);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE); /* swallow taps so the backdrop doesn't close */
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *handle = lv_obj_create(sheet);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 36, 4);
    lv_obj_set_style_radius(handle, 1, 0);
    lv_obj_set_style_bg_color(handle, UI_COL_BORDER, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -4);

    lv_obj_t *bars = ui_signal_bars(sheet, ap->rssi);
    lv_obj_align(bars, LV_ALIGN_TOP_LEFT, 0, 6);
    lv_obj_t *title = ui_label(sheet, &lv_font_montserrat_16, UI_COL_TEXT,
                               ap->ssid[0] ? (const char *)ap->ssid : "(hidden network)");
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 250);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 26, 4);

    lv_obj_t *body = lv_obj_create(sheet);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), SHEET_H - 16 - 28 - 40);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_right(body, 4, 0);

    ui_fmt_mac(buf, sizeof(buf), ap->bssid);
    lv_label_set_text(ui_kv_row(body, "BSSID"), buf);

    int freq = ap->primary == 14 ? 2484 : 2407 + 5 * ap->primary;
    lv_label_set_text_fmt(ui_kv_row(body, "Channel"), "%d  (%d MHz)", ap->primary, freq);

    int bw = wifi_ap_bandwidth_mhz(ap);
    const char *ht = ap->second == WIFI_SECOND_CHAN_ABOVE ? "  HT40+" : ap->second == WIFI_SECOND_CHAN_BELOW ? "  HT40-" : "";
    lv_label_set_text_fmt(ui_kv_row(body, "Bandwidth"), "%d MHz%s", bw, ht);
    if (ap->second != WIFI_SECOND_CHAN_NONE) {
        lv_label_set_text_fmt(ui_kv_row(body, "Secondary ch"), "%d",
                              ap->second == WIFI_SECOND_CHAN_ABOVE ? ap->primary + 4 : ap->primary - 4);
    }

    lv_obj_t *sig = ui_kv_row(body, "Signal");
    lv_label_set_text_fmt(sig, "%d dBm  (%d%%)", ap->rssi, wifi_rssi_quality(ap->rssi));
    lv_obj_set_style_text_color(sig, ui_rssi_color(ap->rssi), 0);

    lv_label_set_text(ui_kv_row(body, "Security"), wifi_auth_str(ap->authmode));
    lv_label_set_text_fmt(ui_kv_row(body, "Cipher"), "%s / %s", wifi_cipher_str(ap->pairwise_cipher),
                          wifi_cipher_str(ap->group_cipher));

    snprintf(buf, sizeof(buf), "%s%s%s%s%s", ap->phy_11b ? "b " : "", ap->phy_11g ? "g " : "", ap->phy_11n ? "n " : "",
             ap->phy_11ax ? "ax " : "", ap->phy_lr ? "LR" : "");
    lv_label_set_text_fmt(ui_kv_row(body, "802.11"), "%s", buf[0] ? buf : "-");
    lv_label_set_text(ui_kv_row(body, "WPS"), ap->wps ? "yes" : "no");
    if (ap->country.cc[0]) {
        lv_label_set_text_fmt(ui_kv_row(body, "Country"), "%c%c  ch %d-%d", ap->country.cc[0], ap->country.cc[1],
                              ap->country.schan, ap->country.schan + ap->country.nchan - 1);
    }
    if (ap->phy_11ax) {
        lv_label_set_text_fmt(ui_kv_row(body, "BSS color"), "%d%s", ap->he_ap.bss_color,
                              ap->he_ap.bss_color_disabled ? " (off)" : "");
    }
    lv_label_set_text(ui_kv_row(body, "FTM responder"), ap->ftm_responder ? "yes" : "no");

    lv_obj_t *close = ui_button_outline(sheet, "Close");
    lv_obj_set_size(close, 120, 32);
    lv_obj_align(close, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(close, on_sheet_close, LV_EVENT_CLICKED, backdrop);

    bool enterprise = ap->authmode == WIFI_AUTH_WPA2_ENTERPRISE || ap->authmode == WIFI_AUTH_WPA3_ENTERPRISE ||
                      ap->authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || ap->authmode == WIFI_AUTH_WPA_ENTERPRISE ||
                      ap->authmode == WIFI_AUTH_WPA3_ENT_192;
    lv_obj_t *conn = ui_button(sheet, "Connect", UI_COL_ACCENT);
    lv_obj_set_size(conn, 160, 32);
    lv_obj_align(conn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(conn, on_sheet_connect, LV_EVENT_CLICKED, backdrop);
    if (enterprise) lv_obj_add_state(conn, LV_STATE_DISABLED); /* EAP needs a username; out of scope */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sheet);
    lv_anim_set_exec_cb(&a, sheet_y_exec);
    lv_anim_set_values(&a, 240, 240 - SHEET_H + 8);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, backdrop);
    lv_anim_set_exec_cb(&a, backdrop_opa_exec);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_60);
    lv_anim_set_duration(&a, 300);
    lv_anim_start(&a);
}

/* ---------------- list ---------------- */

static void on_row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < S.ap_count) open_sheet(&S.aps[idx]);
}

static void build_row(int idx, const uint8_t *connected_bssid)
{
    const wifi_ap_record_t *ap = &S.aps[idx];
    bool is_connected = connected_bssid && memcmp(connected_bssid, ap->bssid, 6) == 0;

    lv_obj_t *row = lv_button_create(S.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, UI_COL_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, UI_RADIUS, 0);
    lv_obj_set_style_border_width(row, is_connected ? 2 : 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(row, UI_COL_GOOD, 0);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *bars = ui_signal_bars(row, ap->rssi);
    lv_obj_align(bars, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *name = ui_label(row, &lv_font_montserrat_14, ap->ssid[0] ? UI_COL_TEXT : UI_COL_TEXT_DIM,
                              ap->ssid[0] ? (const char *)ap->ssid : "(hidden)");
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 196);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 30, 4);

    char mac[20];
    ui_fmt_mac(mac, sizeof(mac), ap->bssid);
    lv_obj_t *sub = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
    lv_label_set_text_fmt(sub, "ch %d  %dMHz  %s  %s", ap->primary, wifi_ap_bandwidth_mhz(ap),
                          wifi_auth_str(ap->authmode), mac);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, 220);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 30, -5);

    lv_obj_t *rssi = ui_label(row, &lv_font_montserrat_14, ui_rssi_color(ap->rssi), "");
    lv_label_set_text_fmt(rssi, "%d", ap->rssi);
    lv_obj_align(rssi, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_t *tag = ui_label(row, &lv_font_montserrat_10, is_connected ? UI_COL_GOOD : UI_COL_TEXT_DIM,
                             is_connected ? "joined" : ap->authmode == WIFI_AUTH_OPEN ? "open" : "dBm");
    lv_obj_align(tag, LV_ALIGN_BOTTOM_RIGHT, -8, -5);
}

static void rebuild_list(void)
{
    sort_aps();

    wifi_conn_info_t info;
    bool connected = wifi_mgr_conn_info(&info);

    int32_t scroll = lv_obj_get_scroll_y(S.list);
    lv_obj_clean(S.list);
    for (int i = 0; i < S.ap_count; i++) {
        build_row(i, connected ? info.bssid : nullptr);
        if (S.first_build && i < 6) ui_anim_enter(lv_obj_get_child(S.list, i), i * 50);
    }
    if (S.ap_count) S.first_build = false;
    lv_obj_scroll_to_y(S.list, scroll, LV_ANIM_OFF);

    if (S.ap_count) lv_obj_add_flag(S.empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(S.empty, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(S.count_lbl, "%d networks", S.ap_count);
}

static void poll(lv_timer_t *)
{
    bool scanning = wifi_mgr_scanning();
    if (scanning) lv_obj_remove_flag(S.spinner, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.spinner, LV_OBJ_FLAG_HIDDEN);

    uint32_t gen = wifi_mgr_scan_gen();
    if (gen != S.seen_gen) {
        S.seen_gen = gen;
        S.ap_count = wifi_mgr_get_aps(S.aps, WIFI_MAX_APS);
        rebuild_list();
    }

    int64_t now = esp_timer_get_time();
    if (!scanning && now - S.last_scan_us > (int64_t)g_settings.scan_interval_s * 1000000) {
        S.last_scan_us = now;
        wifi_mgr_scan_start();
    }
}

static void on_scan_clicked(lv_event_t *)
{
    S.last_scan_us = esp_timer_get_time();
    if (!wifi_mgr_scan_start() && wifi_mgr_state() == WIFI_ST_CONNECTING) {
        ui_toast("Busy connecting, try again in a moment", UI_COL_ACCENT);
    }
}

static void on_sort_clicked(lv_event_t *)
{
    s_sort = (sort_mode_t)((s_sort + 1) % SORT_COUNT);
    lv_label_set_text_fmt(S.sort_btn_lbl, "Sort: %s", SORT_NAMES[s_sort]);
    rebuild_list();
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_color_t col)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 24);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_border_color(b, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, UI_COL_CARD_HI, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_12, col, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_scanner_create(void)
{
    memset(&S, 0, sizeof(S));
    S.first_build = true;

    lv_obj_t *content = ui_screen_create("Scanner", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    S.count_lbl = ui_label(content, &lv_font_montserrat_14, UI_COL_TEXT, "scanning...");
    lv_obj_align(S.count_lbl, LV_ALIGN_TOP_LEFT, 2, 3);

    S.spinner = lv_spinner_create(content);
    lv_obj_set_size(S.spinner, 18, 18);
    lv_obj_set_style_arc_width(S.spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(S.spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(S.spinner, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(S.spinner, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_align(S.spinner, LV_ALIGN_TOP_LEFT, 100, 3);

    S.scan_btn = small_button(content, "Scan", UI_COL_TEXT);
    lv_obj_align(S.scan_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.scan_btn, on_scan_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *sort = small_button(content, "", UI_COL_TEXT);
    S.sort_btn_lbl = lv_obj_get_child(sort, 0);
    lv_label_set_text_fmt(S.sort_btn_lbl, "Sort: %s", SORT_NAMES[s_sort]);
    lv_obj_align(sort, LV_ALIGN_TOP_RIGHT, -52, 0);
    lv_obj_add_event_cb(sort, on_sort_clicked, LV_EVENT_CLICKED, nullptr);

    S.list = lv_obj_create(content);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_size(S.list, LV_PCT(100), UI_CONTENT_H - 12 - TOOLBAR_H - 2);
    lv_obj_align(S.list, LV_ALIGN_TOP_LEFT, 0, TOOLBAR_H + 2);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 4, 0);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_ACTIVE);

    S.empty = lv_obj_create(content);
    lv_obj_remove_style_all(S.empty);
    lv_obj_set_size(S.empty, 200, 120);
    lv_obj_align(S.empty, LV_ALIGN_CENTER, 0, 14);
    lv_obj_remove_flag(S.empty, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *radar = lv_spinner_create(S.empty);
    lv_spinner_set_anim_params(radar, 1400, 90);
    lv_obj_set_size(radar, 48, 48);
    lv_obj_align(radar, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_arc_width(radar, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(radar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(radar, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(radar, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_t *hint = ui_label(S.empty, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "Looking for networks...");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* Show the last results immediately, then refresh. */
    S.seen_gen = wifi_mgr_scan_gen();
    S.ap_count = wifi_mgr_get_aps(S.aps, WIFI_MAX_APS);
    rebuild_list();
    if (!S.ap_count) lv_label_set_text(S.count_lbl, "scanning...");

    S.last_scan_us = esp_timer_get_time();
    wifi_mgr_scan_start();
    ui_screen_timer(content, poll, 250, nullptr);
    return lv_obj_get_screen(content);
}
