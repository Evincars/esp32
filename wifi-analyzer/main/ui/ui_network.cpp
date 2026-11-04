#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "oui.h"
#include "ui.h"
#include "wifi_mgr.h"

#define RSSI_POINTS 60

static struct {
    lv_obj_t *status_tab;
    lv_obj_t *lan_tab;
    wifi_state_t shown_state;
    bool built;

    /* connected view */
    lv_obj_t *ssid, *bars, *rssi, *chart;
    lv_chart_series_t *series;
    lv_obj_t *v_bssid, *v_channel, *v_security, *v_phy, *v_ip, *v_mask, *v_gw, *v_dns1, *v_dns2, *v_ip6, *v_mac,
        *v_host, *v_uptime;

    /* LAN tab */
    lv_obj_t *lan_btn, *lan_bar, *lan_status, *lan_list;
    uint32_t lan_seen_gen;
} S;

/* ---------------- status tab ---------------- */

static void on_disconnect(lv_event_t *)
{
    wifi_mgr_disconnect();
    ui_toast("Disconnected", UI_COL_ACCENT);
}

static void on_scan(lv_event_t *)
{
    ui_go(UI_SCR_SCANNER);
}

static void on_hidden(lv_event_t *)
{
    ui_connect_flow("", true);
}

static void on_reconnect(lv_event_t *)
{
    wifi_mgr_autoconnect();
}

static void on_retry(lv_event_t *)
{
    char ssid[33];
    if (wifi_mgr_target_ssid(ssid, sizeof(ssid))) ui_connect_flow(ssid, true);
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    return row;
}

static void build_offline(lv_obj_t *tab, wifi_state_t st)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tab, 6, 0);

    if (st == WIFI_ST_CONNECTING) {
        lv_obj_t *sp = lv_spinner_create(tab);
        lv_obj_set_size(sp, 56, 56);
        lv_obj_set_size(sp, 40, 40);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, UI_COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, UI_COL_BORDER, LV_PART_MAIN);
        lv_obj_t *t = ui_label(tab, &lv_font_montserrat_14, UI_COL_TEXT, wifi_mgr_state_text());
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(t, 290, 0);
        lv_obj_t *row = button_row(tab);
        lv_obj_t *b = ui_button_outline(row, "Cancel");
        lv_obj_add_event_cb(b, on_disconnect, LV_EVENT_CLICKED, nullptr);
        return;
    }

    if (st == WIFI_ST_FAILED) {
        ui_caption(tab, "CONNECTION FAILED");
        lv_obj_t *t = ui_label(tab, &lv_font_montserrat_16, UI_COL_BAD, wifi_mgr_state_text());
        ui_anim_enter(t, 0);
        lv_obj_t *row = button_row(tab);
        lv_obj_add_event_cb(ui_button(row, "Retry", UI_COL_ACCENT), on_retry, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(ui_button_outline(row, "Scan"), on_scan, LV_EVENT_CLICKED, nullptr);
        return;
    }

    lv_obj_t *t = ui_label(tab, &lv_font_montserrat_16, UI_COL_TEXT, "Not connected");
    ui_anim_enter(t, 0);
    lv_obj_t *hint = ui_label(tab, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "Pick a network in the Scanner to join it");
    ui_anim_enter(hint, 60);
    lv_obj_t *row = button_row(tab);
    lv_obj_add_event_cb(ui_button(row, "Scan", UI_COL_ACCENT), on_scan, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_button_outline(row, "Hidden"), on_hidden, LV_EVENT_CLICKED, nullptr);
    if (wifi_mgr_known_count() > 0) {
        lv_obj_add_event_cb(ui_button_outline(row, "Reconnect"), on_reconnect, LV_EVENT_CLICKED, nullptr);
    }
    ui_anim_enter(row, 120);
}

static void build_connected(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(tab, 6, 0);

    /* hero card: SSID, live RSSI and its history */
    lv_obj_t *hero = ui_card(tab);
    lv_obj_set_size(hero, LV_PCT(100), 88);
    S.bars = ui_signal_bars(hero, -100);
    lv_obj_align(S.bars, LV_ALIGN_TOP_LEFT, 0, 0);
    S.ssid = ui_label(hero, &lv_font_montserrat_16, UI_COL_TEXT, "");
    lv_label_set_long_mode(S.ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.ssid, 190);
    lv_obj_align(S.ssid, LV_ALIGN_TOP_LEFT, 26, -2);
    S.rssi = ui_label(hero, &lv_font_montserrat_16, UI_COL_GOOD, "");
    lv_obj_align(S.rssi, LV_ALIGN_TOP_RIGHT, 0, -2);

    S.chart = lv_chart_create(hero);
    lv_obj_set_size(S.chart, LV_PCT(100), 50);
    lv_obj_align(S.chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(S.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(S.chart, RSSI_POINTS);
    lv_chart_set_range(S.chart, LV_CHART_AXIS_PRIMARY_Y, -100, -20);
    lv_chart_set_div_line_count(S.chart, 3, 0);
    lv_chart_set_update_mode(S.chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(S.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(S.chart, 0, 0);
    lv_obj_set_style_pad_all(S.chart, 0, 0);
    lv_obj_set_style_line_color(S.chart, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_line_width(S.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(S.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(S.chart, 0, LV_PART_INDICATOR);
    S.series = lv_chart_add_series(S.chart, UI_COL_GOOD, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(S.chart, S.series, LV_CHART_POINT_NONE);
    ui_anim_enter(hero, 0);

    lv_obj_t *card = ui_card(tab);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    S.v_ip = ui_kv_row(card, "IPv4 address");
    lv_obj_set_style_text_color(S.v_ip, UI_COL_GOOD, 0);
    S.v_mask = ui_kv_row(card, "Netmask");
    S.v_gw = ui_kv_row(card, "Gateway");
    S.v_dns1 = ui_kv_row(card, "DNS 1");
    S.v_dns2 = ui_kv_row(card, "DNS 2");
    S.v_ip6 = ui_kv_row(card, "IPv6 link-local");
    lv_obj_set_style_text_font(S.v_ip6, &lv_font_montserrat_10, 0);
    S.v_mac = ui_kv_row(card, "My MAC");
    S.v_host = ui_kv_row(card, "Hostname");
    S.v_bssid = ui_kv_row(card, "AP BSSID");
    S.v_channel = ui_kv_row(card, "Channel");
    S.v_security = ui_kv_row(card, "Security");
    S.v_phy = ui_kv_row(card, "PHY mode");
    S.v_uptime = ui_kv_row(card, "Connected for");
    ui_anim_enter(card, 90);

    lv_obj_t *row = button_row(tab);
    lv_obj_add_event_cb(ui_button(row, "Disconnect", UI_COL_BAD), on_disconnect, LV_EVENT_CLICKED, nullptr);
}

static void update_connected(void)
{
    wifi_conn_info_t info;
    if (!wifi_mgr_conn_info(&info)) return;
    char buf[64];

    lv_label_set_text(S.ssid, info.ssid);
    lv_label_set_text_fmt(S.rssi, "%d dBm", info.rssi);
    lv_obj_set_style_text_color(S.rssi, ui_rssi_color(info.rssi), 0);
    ui_signal_bars_set(S.bars, info.rssi);
    lv_chart_set_series_color(S.chart, S.series, ui_rssi_color(info.rssi));
    lv_chart_set_next_value(S.chart, S.series, info.rssi);

    lv_label_set_text_fmt(S.v_ip, IPSTR, IP2STR(&info.ip.ip));
    lv_label_set_text_fmt(S.v_mask, IPSTR, IP2STR(&info.ip.netmask));
    lv_label_set_text_fmt(S.v_gw, IPSTR, IP2STR(&info.ip.gw));
    lv_label_set_text_fmt(S.v_dns1, IPSTR, IP2STR(&info.dns[0]));
    if (info.dns[1].addr) lv_label_set_text_fmt(S.v_dns2, IPSTR, IP2STR(&info.dns[1]));
    if (info.has_ip6) lv_label_set_text_fmt(S.v_ip6, IPV6STR, IPV62STR(info.ip6));
    ui_fmt_mac(buf, sizeof(buf), info.mac);
    lv_label_set_text(S.v_mac, buf);
    lv_label_set_text(S.v_host, info.hostname);
    ui_fmt_mac(buf, sizeof(buf), info.bssid);
    lv_label_set_text(S.v_bssid, buf);
    lv_label_set_text_fmt(S.v_channel, "%d%s", info.channel,
                          info.second == WIFI_SECOND_CHAN_ABOVE ? " (HT40+)" : info.second == WIFI_SECOND_CHAN_BELOW ? " (HT40-)" : "");
    lv_label_set_text(S.v_security, wifi_auth_str(info.authmode));
    lv_label_set_text_fmt(S.v_phy, "802.11%s%s%s%s", info.phy_11b ? "b" : "", info.phy_11g ? "g" : "",
                          info.phy_11n ? "n" : "", info.phy_11ax ? "ax" : "");

    int64_t secs = (esp_timer_get_time() - info.connected_since_us) / 1000000;
    if (secs >= 3600) lv_label_set_text_fmt(S.v_uptime, "%dh %02dm", (int)(secs / 3600), (int)(secs / 60 % 60));
    else lv_label_set_text_fmt(S.v_uptime, "%dm %02ds", (int)(secs / 60), (int)(secs % 60));
}

static void rebuild_status(wifi_state_t st)
{
    lv_obj_clean(S.status_tab);
    S.shown_state = st;
    if (st == WIFI_ST_CONNECTED) {
        build_connected(S.status_tab);
        update_connected();
    } else {
        build_offline(S.status_tab, st);
    }
}

/* ---------------- LAN tab ---------------- */

static void on_host_clicked(lv_event_t *e)
{
    ui_ports_open((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void rebuild_lan_list(void)
{
    static lan_host_t hosts[LAN_MAX_HOSTS];
    int n = wifi_mgr_lan_hosts(hosts, LAN_MAX_HOSTS);
    int old = lv_obj_get_child_count(S.lan_list);
    lv_obj_clean(S.lan_list);
    for (int i = 0; i < n; i++) {
        const lan_host_t &h = hosts[i];
        lv_obj_t *row = lv_button_create(S.lan_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 32);
        lv_obj_set_style_bg_color(row, UI_COL_CARD, 0);
        lv_obj_set_style_bg_color(row, UI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_border_color(row, h.is_self || h.is_gateway ? UI_COL_ACCENT : UI_COL_BORDER, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        if (!h.is_self) lv_obj_add_event_cb(row, on_host_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)h.ip);

        esp_ip4_addr_t ip = {h.ip};
        lv_obj_t *ipl = ui_label(row, &lv_font_montserrat_14, UI_COL_TEXT, "");
        lv_label_set_text_fmt(ipl, IPSTR, IP2STR(&ip));
        lv_obj_align(ipl, LV_ALIGN_TOP_LEFT, 0, 0);

        char mac[20];
        ui_fmt_mac(mac, sizeof(mac), h.mac);
        const char *vendor = oui_vendor(h.mac);
        lv_obj_t *macl = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(macl, "%s%s%s", mac, vendor ? "  " : "", vendor ? vendor : "");
        lv_obj_align(macl, LV_ALIGN_BOTTOM_LEFT, 0, -1);

        lv_obj_t *tag = ui_label(row, &lv_font_montserrat_10, h.is_self || h.is_gateway ? UI_COL_ACCENT : UI_COL_TEXT_DIM,
                                 h.is_self ? "THIS DEVICE" : h.is_gateway ? "GATEWAY  scan >" : "scan >");
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
        if (i >= old) ui_anim_enter(row, 0);
    }
}

static void on_lan_scan(lv_event_t *)
{
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
        ui_toast("Connect to a network first", UI_COL_ACCENT);
        return;
    }
    if (wifi_mgr_lan_scan_start()) {
        lv_obj_clean(S.lan_list);
    }
}

static void update_lan(void)
{
    int pct = 0;
    bool busy = wifi_mgr_lan_scanning(&pct);
    lv_bar_set_value(S.lan_bar, pct, LV_ANIM_ON);
    if (busy) {
        lv_obj_add_state(S.lan_btn, LV_STATE_DISABLED);
        lv_label_set_text_fmt(S.lan_status, "Probing hosts... %d%%", pct);
    } else {
        lv_obj_remove_state(S.lan_btn, LV_STATE_DISABLED);
        int n = lv_obj_get_child_count(S.lan_list);
        if (wifi_mgr_state() != WIFI_ST_CONNECTED) lv_label_set_text(S.lan_status, "Not connected");
        else if (n) lv_label_set_text_fmt(S.lan_status, "%d hosts, tap to port-scan", n);
        else lv_label_set_text(S.lan_status, "ARP sweep of the local /24");
    }

    uint32_t gen = wifi_mgr_lan_gen();
    if (gen != S.lan_seen_gen) {
        S.lan_seen_gen = gen;
        rebuild_lan_list();
    }
}

static void build_lan(lv_obj_t *tab)
{
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    S.lan_btn = ui_button(tab, "Discover", UI_COL_ACCENT);
    lv_obj_set_size(S.lan_btn, 90, 28);
    lv_obj_set_style_pad_all(S.lan_btn, 0, 0);
    lv_obj_align(S.lan_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.lan_btn, on_lan_scan, LV_EVENT_CLICKED, nullptr);

    S.lan_status = ui_label(tab, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_obj_align(S.lan_status, LV_ALIGN_TOP_LEFT, 0, 2);

    S.lan_bar = ui_bar(tab, 170, 2);
    lv_obj_align(S.lan_bar, LV_ALIGN_TOP_LEFT, 0, 22);

    S.lan_list = lv_obj_create(tab);
    lv_obj_remove_style_all(S.lan_list);
    lv_obj_set_size(S.lan_list, LV_PCT(100), UI_CONTENT_H - 28 - 12 - 36);
    lv_obj_align(S.lan_list, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_set_flex_flow(S.lan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.lan_list, 4, 0);
    lv_obj_set_scrollbar_mode(S.lan_list, LV_SCROLLBAR_MODE_ACTIVE);

    S.lan_seen_gen = ~wifi_mgr_lan_gen(); /* force initial fill from previous results */
}

/* ---------------- screen ---------------- */

static void poll(lv_timer_t *)
{
    static uint32_t tick;
    wifi_state_t st = wifi_mgr_state();
    if (st != S.shown_state) {
        if (st == WIFI_ST_CONNECTED) ui_toast("Connected", UI_COL_GOOD);
        rebuild_status(st);
    } else if (st == WIFI_ST_CONNECTED && ++tick % 2 == 0) {
        update_connected();
    }
    update_lan();
}

static void style_tabview(lv_obj_t *tv)
{
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, UI_COL_HEADER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    uint32_t n = lv_obj_get_child_count(bar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(bar, i);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(btn, UI_COL_TEXT_DIM, 0);
        lv_obj_set_style_text_color(btn, UI_COL_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, UI_COL_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
    }
    lv_obj_t *cont = lv_tabview_get_content(tv);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
}

lv_obj_t *ui_network_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Network", true);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tv = lv_tabview_create(content);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_size(tv, 28);
    S.status_tab = lv_tabview_add_tab(tv, "Status");
    S.lan_tab = lv_tabview_add_tab(tv, "LAN hosts");
    style_tabview(tv);
    lv_obj_set_style_pad_all(S.status_tab, 6, 0);
    lv_obj_set_style_pad_all(S.lan_tab, 6, 0);

    build_lan(S.lan_tab);
    rebuild_status(wifi_mgr_state());
    update_lan();
    ui_screen_timer(content, poll, 500, nullptr);
    return lv_obj_get_screen(content);
}
