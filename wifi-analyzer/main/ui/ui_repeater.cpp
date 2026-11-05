#include <cstdio>
#include <cstring>

#include "app_settings.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "netid.h"
#include "oui.h"
#include "repeater.h"
#include "ui.h"
#include "wifi_mgr.h"

static struct {
    lv_obj_t *state, *btn, *btn_lbl;
    lv_obj_t *v_up, *v_gw, *v_clients, *v_ssid, *v_sec;
    lv_obj_t *v_wan, *v_lan;
    lv_obj_t *v_mac_sta, *v_mac_ap, *v_mac_bt, *v_mac_label;
    lv_obj_t *list;
    int shown_clients;
} S;

static void fmt_bytes(char *buf, size_t n, uint64_t b)
{
    if (b < 1024) snprintf(buf, n, "%llu B", (unsigned long long)b);
    else if (b < 1024 * 1024) snprintf(buf, n, "%.1f KB", b / 1024.0);
    else if (b < 1024ULL * 1024 * 1024) snprintf(buf, n, "%.2f MB", b / (1024.0 * 1024));
    else snprintf(buf, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

static void show_mac(lv_obj_t *lbl, wifi_interface_t ifx, esp_mac_type_t bt_type, bool is_bt)
{
    uint8_t mac[6] = {};
    if (is_bt) esp_read_mac(mac, bt_type);
    else esp_wifi_get_mac(ifx, mac);
    char m[20];
    ui_fmt_mac(m, sizeof(m), mac);
    const char *v = oui_vendor(mac);
    lv_label_set_text_fmt(lbl, "%s  %s", m, v ? v : "");
}

static void rebuild_clients(void)
{
    lv_obj_clean(S.list);
    repeater_client_t c[REP_MAX_CLIENTS];
    int n = repeater_clients(c, REP_MAX_CLIENTS);
    S.shown_clients = n;
    if (n == 0) {
        ui_label(S.list, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "no clients connected");
        return;
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(S.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        esp_ip4_addr_t ip = {c[i].ip};
        lv_obj_t *ipl = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, "");
        if (c[i].ip) lv_label_set_text_fmt(ipl, IPSTR, IP2STR(&ip));
        else lv_label_set_text(ipl, "(no lease)");
        lv_obj_align(ipl, LV_ALIGN_TOP_LEFT, 0, 0);

        char m[20];
        ui_fmt_mac(m, sizeof(m), c[i].mac);
        const char *v = oui_vendor(c[i].mac);
        lv_obj_t *macl = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(macl, "%s  %s%s", m, v ? v : "", (c[i].mac[0] & 0x02) ? " (random)" : "");
        lv_obj_align(macl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_t *rssi = ui_label(row, &lv_font_montserrat_12, ui_rssi_color(c[i].rssi), "");
        lv_label_set_text_fmt(rssi, "%d dBm", c[i].rssi);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void update(lv_timer_t *)
{
    repeater_status_t st;
    repeater_status(&st);
    bool on = st.running;

    lv_label_set_text(S.state, on ? "Running" : "Stopped");
    lv_obj_set_style_text_color(S.state, on ? UI_COL_GOOD : UI_COL_TEXT_DIM, 0);
    lv_label_set_text(S.btn_lbl, on ? "Stop repeater" : "Start repeater");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);

    lv_label_set_text(S.v_ssid, g_settings.rep_ssid);
    lv_label_set_text(S.v_sec, g_settings.rep_pass[0] ? "WPA2" : "Open");

    if (st.upstream_up) {
        lv_label_set_text_fmt(S.v_up, "%s  ch%d", st.upstream_ssid, st.channel);
        lv_obj_set_style_text_color(S.v_up, UI_COL_GOOD, 0);
    } else {
        lv_label_set_text(S.v_up, "not connected");
        lv_obj_set_style_text_color(S.v_up, UI_COL_BAD, 0);
    }

    if (on && st.ap_ip) {
        esp_ip4_addr_t ip = {st.ap_ip};
        lv_label_set_text_fmt(S.v_gw, IPSTR, IP2STR(&ip));
    } else {
        lv_label_set_text(S.v_gw, "-");
    }
    lv_label_set_text_fmt(S.v_clients, "%d / %d", st.clients, REP_MAX_CLIENTS);

    char rx[16], tx[16];
    fmt_bytes(rx, sizeof(rx), st.wan_rx);
    fmt_bytes(tx, sizeof(tx), st.wan_tx);
    lv_label_set_text_fmt(S.v_wan, LV_SYMBOL_DOWN " %s   " LV_SYMBOL_UP " %s", rx, tx);
    fmt_bytes(rx, sizeof(rx), st.lan_rx);
    fmt_bytes(tx, sizeof(tx), st.lan_tx);
    lv_label_set_text_fmt(S.v_lan, LV_SYMBOL_DOWN " %s   " LV_SYMBOL_UP " %s", rx, tx);

    show_mac(S.v_mac_sta, WIFI_IF_STA, ESP_MAC_WIFI_STA, false);
    show_mac(S.v_mac_ap, WIFI_IF_AP, ESP_MAC_WIFI_SOFTAP, false);
    show_mac(S.v_mac_bt, WIFI_IF_STA, ESP_MAC_BT, true);
    lv_label_set_text(S.v_mac_label, netid_label());

    if (st.clients != S.shown_clients) rebuild_clients();
}

static void on_toggle(lv_event_t *)
{
    if (repeater_running()) {
        repeater_stop();
    } else if (!repeater_start(g_settings.rep_ssid, g_settings.rep_pass)) {
        ui_toast("Could not start repeater", UI_COL_BAD);
    } else {
        ui_toast(LV_SYMBOL_OK "  Repeater on - connect to it", UI_COL_GOOD);
    }
    update(nullptr);
}

static void on_ssid_done(const char *text, void *)
{
    if (text && text[0]) {
        strlcpy(g_settings.rep_ssid, text, sizeof(g_settings.rep_ssid));
        app_settings_save();
    }
    ui_go(UI_SCR_REPEATER);
}

static void on_pass_done(const char *text, void *)
{
    size_t len = text ? strlen(text) : 0;
    if (len && len < 8) {
        ui_toast("WPA2 needs 8+ chars (empty = open)", UI_COL_BAD);
        return;
    }
    strlcpy(g_settings.rep_pass, text ? text : "", sizeof(g_settings.rep_pass));
    app_settings_save();
    ui_go(UI_SCR_REPEATER);
}

static void on_edit_ssid(lv_event_t *)
{
    ui_text_input("Repeater name", "repeater-esp32-s3", g_settings.rep_ssid, false, on_ssid_done, nullptr,
                  UI_SCR_REPEATER);
}

static void on_edit_pass(lv_event_t *)
{
    ui_text_input("Repeater password", "8+ chars, empty = open", g_settings.rep_pass, true, on_pass_done, nullptr,
                  UI_SCR_REPEATER);
}

static void on_randomize(lv_event_t *)
{
    if (repeater_running()) {
        ui_toast("Stop the repeater first", UI_COL_BAD);
        return;
    }
    ui_toast("Randomizing MACs...", UI_COL_ACCENT);
    netid_randomize();
    update(nullptr);
}

static void on_restore(lv_event_t *)
{
    if (repeater_running()) {
        ui_toast("Stop the repeater first", UI_COL_BAD);
        return;
    }
    netid_restore();
    ui_toast("Factory MACs restored", UI_COL_GOOD);
    update(nullptr);
}

static lv_obj_t *section_card(lv_obj_t *parent, const char *cap)
{
    ui_caption(parent, cap);
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    return card;
}

static lv_obj_t *small_btn(lv_obj_t *parent, const char *txt, lv_color_t col, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(parent, txt, col);
    lv_obj_set_height(b, 28);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

lv_obj_t *ui_repeater_create(void)
{
    memset(&S, 0, sizeof(S));
    S.shown_clients = -1;
    lv_obj_t *content = ui_screen_create("Repeater", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 5, 0);

    lv_obj_t *status = section_card(content, "STATUS");
    S.state = ui_kv_row(status, "State");
    S.v_up = ui_kv_row(status, "Upstream");
    S.v_ssid = ui_kv_row(status, "Repeater SSID");
    S.v_sec = ui_kv_row(status, "Security");
    S.v_gw = ui_kv_row(status, "Gateway");
    S.v_clients = ui_kv_row(status, "Clients");

    lv_obj_t *names = lv_obj_create(content);
    lv_obj_remove_style_all(names);
    lv_obj_set_size(names, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(names, 6, 0);
    small_btn(names, "Name", UI_COL_CARD_HI, on_edit_ssid);
    small_btn(names, "Password", UI_COL_CARD_HI, on_edit_pass);
    lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_child(names, 0), 0), UI_COL_TEXT, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_child(names, 1), 0), UI_COL_TEXT, 0);

    lv_obj_t *thr = section_card(content, "THROUGHPUT");
    S.v_wan = ui_kv_row(thr, "To home Wi-Fi");
    S.v_lan = ui_kv_row(thr, "Through repeater");

    lv_obj_t *mac = section_card(content, "MAC IDENTITY");
    S.v_mac_sta = ui_kv_row(mac, "Station");
    lv_obj_set_style_text_font(S.v_mac_sta, &lv_font_montserrat_10, 0);
    S.v_mac_ap = ui_kv_row(mac, "SoftAP");
    lv_obj_set_style_text_font(S.v_mac_ap, &lv_font_montserrat_10, 0);
    S.v_mac_bt = ui_kv_row(mac, "Bluetooth");
    lv_obj_set_style_text_font(S.v_mac_bt, &lv_font_montserrat_10, 0);
    S.v_mac_label = ui_kv_row(mac, "Identity");

    lv_obj_t *macbtns = lv_obj_create(content);
    lv_obj_remove_style_all(macbtns);
    lv_obj_set_size(macbtns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(macbtns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(macbtns, 6, 0);
    small_btn(macbtns, "Randomize (HP)", UI_COL_ACCENT, on_randomize);
    small_btn(macbtns, "Factory", UI_COL_CARD_HI, on_restore);
    lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_child(macbtns, 1), 0), UI_COL_TEXT, 0);

    ui_caption(content, "CLIENTS");
    S.list = ui_card(content);
    lv_obj_set_size(S.list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(S.list, 36, 0);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 4, 0);

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, LV_PCT(100), 34);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    rebuild_clients();
    lv_timer_t *t = ui_screen_timer(content, update, 800, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
