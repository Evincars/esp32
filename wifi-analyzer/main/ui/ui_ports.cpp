#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "oui.h"
#include "portscan.h"
#include "ui.h"
#include "wifi_mgr.h"

#define TARGET_ALL 0xFFFFFFFFu
#define TARGET_CUSTOM 0u
#define MAX_OPTS (LAN_MAX_HOSTS + 2)

/* survive screen rebuilds */
static uint32_t s_target = TARGET_ALL;
static uint32_t s_custom_ip;
static ps_profile_t s_profile = PS_PROFILE_TOP;
static uint32_t s_preset_ip;
static bool s_from_network;

static struct {
    lv_obj_t *target_dd, *profile_dd, *btn, *btn_lbl, *status, *bar, *list;
    uint32_t opts[MAX_OPTS];
    int nopts;
    uint32_t seen_scan_gen, seen_lan_gen;
    bool shown_running;
} S;

/* copies of the scanner's data for building the view; in PSRAM */
typedef struct {
    lan_host_t hosts[LAN_MAX_HOSTS];
    ps_open_t open[PS_MAX_OPEN];
    ps_host_t ps_hosts[PS_MAX_TARGETS];
    uint32_t ips[PS_MAX_TARGETS];
    char opts[MAX_OPTS * 28];
} scratch_t;

static scratch_t *W;
#define s_hosts (W->hosts)
#define s_open (W->open)
#define s_ps_hosts (W->ps_hosts)

static void ip_text(char *buf, size_t len, uint32_t ip)
{
    esp_ip4_addr_t a = {ip};
    snprintf(buf, len, IPSTR, IP2STR(&a));
}

/* ---------------- target list ---------------- */

static void rebuild_targets(void)
{
    int n = wifi_mgr_lan_hosts(s_hosts, LAN_MAX_HOSTS);
    char *opts = W->opts;
    const size_t opts_len = sizeof(W->opts);
    size_t k = 0;
    S.nopts = 0;
    int others = 0;
    for (int i = 0; i < n; i++) others += !s_hosts[i].is_self;

    k += snprintf(opts + k, opts_len - k, "All LAN hosts (%d)", others);
    S.opts[S.nopts++] = TARGET_ALL;
    for (int i = 0; i < n && S.nopts < MAX_OPTS - 1; i++) {
        if (s_hosts[i].is_self) continue;
        char ip[16];
        ip_text(ip, sizeof(ip), s_hosts[i].ip);
        k += snprintf(opts + k, opts_len - k, "\n%s%s", ip, s_hosts[i].is_gateway ? "  gw" : "");
        S.opts[S.nopts++] = s_hosts[i].ip;
    }
    if (s_custom_ip) {
        bool listed = false;
        for (int i = 1; i < S.nopts; i++) listed |= S.opts[i] == s_custom_ip;
        if (!listed) {
            char ip[16];
            ip_text(ip, sizeof(ip), s_custom_ip);
            k += snprintf(opts + k, opts_len - k, "\n%s", ip);
            S.opts[S.nopts++] = s_custom_ip;
        }
    }
    snprintf(opts + k, opts_len - k, "\nOther IP...");
    S.opts[S.nopts++] = TARGET_CUSTOM;

    lv_dropdown_set_options(S.target_dd, opts);
    for (int i = 0; i < S.nopts; i++) {
        if (S.opts[i] == s_target) lv_dropdown_set_selected(S.target_dd, i);
    }
}

static void on_custom_ip(const char *text, void *)
{
    ip4_addr_t a;
    if (!ip4addr_aton(text, &a) || a.addr == 0) {
        ui_toast("Not a valid IPv4 address", UI_COL_BAD);
        return;
    }
    s_custom_ip = a.addr;
    s_target = a.addr;
    ui_go(UI_SCR_PORTS);
}

static void on_target(lv_event_t *)
{
    uint32_t sel = S.opts[lv_dropdown_get_selected(S.target_dd)];
    if (sel == TARGET_CUSTOM) {
        char ip[16] = "";
        if (s_custom_ip) ip_text(ip, sizeof(ip), s_custom_ip);
        ui_text_input("Target IP", "e.g. 192.168.1.10", ip, false, on_custom_ip, nullptr, UI_SCR_PORTS);
        return;
    }
    s_target = sel;
}

static void on_profile(lv_event_t *)
{
    s_profile = (ps_profile_t)lv_dropdown_get_selected(S.profile_dd);
}

/* ---------------- scanning ---------------- */

static void start_scan(void)
{
    uint32_t *ips = W->ips;
    int n = 0;
    if (s_target == TARGET_ALL) {
        int hn = wifi_mgr_lan_hosts(s_hosts, LAN_MAX_HOSTS);
        for (int i = 0; i < hn && n < PS_MAX_TARGETS; i++) {
            if (!s_hosts[i].is_self) ips[n++] = s_hosts[i].ip;
        }
        if (!n) {
            ui_toast(wifi_mgr_state() == WIFI_ST_CONNECTED ? "No hosts yet - wait for discovery" : "Connect to a network first",
                     UI_COL_ACCENT);
            if (wifi_mgr_state() == WIFI_ST_CONNECTED) wifi_mgr_lan_scan_start();
            return;
        }
    } else {
        ips[n++] = s_target;
    }
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
        ui_toast("Connect to a network first", UI_COL_ACCENT);
        return;
    }
    portscan_start(ips, n, s_profile);
}

static void on_button(lv_event_t *)
{
    ps_status_t st;
    portscan_status(&st);
    if (st.running) portscan_stop();
    else start_scan();
}

/* ---------------- results ---------------- */

static void add_port_row(const ps_open_t &o)
{
    lv_obj_t *row = lv_obj_create(S.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_border_color(row, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);

    lv_obj_t *port = ui_label(row, &lv_font_montserrat_12, UI_COL_ACCENT, "");
    lv_label_set_text_fmt(port, "%u/tcp", o.port);
    lv_obj_t *svc = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, portscan_service(o.port)[0] ? portscan_service(o.port) : "unknown");
    lv_obj_set_pos(svc, 64, 0);
    lv_obj_t *rtt = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
    lv_label_set_text_fmt(rtt, "%u ms", o.rtt_ms);
    lv_obj_align(rtt, LV_ALIGN_TOP_RIGHT, 0, 1);
    if (o.banner[0]) {
        lv_obj_t *b = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, o.banner);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_pos(b, 0, 16);
    }
}

static void rebuild_results(void)
{
    int no = portscan_results(s_open, PS_MAX_OPEN);
    int nh = portscan_hosts(s_ps_hosts, PS_MAX_TARGETS);
    int32_t scroll = lv_obj_get_scroll_y(S.list);
    lv_obj_clean(S.list);

    int hn = wifi_mgr_lan_hosts(s_hosts, LAN_MAX_HOSTS);
    int quiet = 0;
    for (int h = 0; h < nh; h++) {
        const ps_host_t &ph = s_ps_hosts[h];
        if (!ph.open) {
            quiet += ph.done;
            continue;
        }
        char ip[16];
        ip_text(ip, sizeof(ip), ph.ip);
        const char *vendor = nullptr;
        for (int i = 0; i < hn; i++) {
            if (s_hosts[i].ip == ph.ip) vendor = oui_vendor(s_hosts[i].mac);
        }
        lv_obj_t *hdr = lv_obj_create(S.list);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(hdr, 4, 0);
        ui_label(hdr, &lv_font_montserrat_14, UI_COL_TEXT, ip);
        lv_obj_t *m = ui_label(hdr, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(m, "%s%s%u open  %u closed  %u filtered", vendor ? vendor : "", vendor ? "  " : "", ph.open,
                              ph.closed, ph.filtered);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);
        for (int i = 0; i < no; i++) {
            if (s_open[i].ip == ph.ip) add_port_row(s_open[i]);
        }
    }
    if (quiet) {
        lv_obj_t *q = ui_label(S.list, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(q, "%d host%s with no open ports", quiet, quiet == 1 ? "" : "s");
    }
    if (!nh) {
        ui_label(S.list, &lv_font_montserrat_12, UI_COL_TEXT_DIM,
                 "TCP connect scan of common service ports.\nOpen ports are probed for a banner\n(SSH/FTP greeting, HTTP Server header).");
    }
    lv_obj_scroll_to_y(S.list, scroll, LV_ANIM_OFF);
}

static void poll(lv_timer_t *)
{
    ps_status_t st;
    portscan_status(&st);
    char ip[16];
    if (st.running) {
        ip_text(ip, sizeof(ip), st.current_ip);
        lv_label_set_text_fmt(S.status, "%s  host %d/%d  port %d/%d  %d open", ip, st.hosts_done + 1, st.hosts_total,
                              st.ports_done, st.ports_total, st.open_count);
        int total = st.hosts_total * st.ports_total;
        int done = st.hosts_done * st.ports_total + st.ports_done;
        lv_bar_set_value(S.bar, total ? done * 100 / total : 0, LV_ANIM_ON);
    } else if (st.hosts_total) {
        lv_label_set_text_fmt(S.status, "%d open on %d host%s in %lu.%lu s", st.open_count, st.hosts_done,
                              st.hosts_done == 1 ? "" : "s", (unsigned long)(st.elapsed_ms / 1000),
                              (unsigned long)(st.elapsed_ms / 100 % 10));
        lv_bar_set_value(S.bar, 100, LV_ANIM_OFF);
    } else {
        int pct;
        if (wifi_mgr_lan_scanning(&pct)) lv_label_set_text_fmt(S.status, "Discovering hosts... %d%%", pct);
        else if (wifi_mgr_state() != WIFI_ST_CONNECTED) lv_label_set_text(S.status, "Not connected");
        else lv_label_set_text(S.status, "Pick a target and press Scan");
        lv_bar_set_value(S.bar, 0, LV_ANIM_OFF);
    }
    if (st.running != S.shown_running) {
        S.shown_running = st.running;
        lv_label_set_text(S.btn_lbl, st.running ? "Stop" : "Scan");
        lv_obj_set_style_bg_color(S.btn, st.running ? UI_COL_BAD : UI_COL_ACCENT, 0);
    }

    uint32_t lg = wifi_mgr_lan_gen();
    if (lg != S.seen_lan_gen && !wifi_mgr_lan_scanning(nullptr)) {
        S.seen_lan_gen = lg;
        rebuild_targets();
    }
    uint32_t g = portscan_gen();
    if (g != S.seen_scan_gen) {
        S.seen_scan_gen = g;
        rebuild_results();
    }
}

static void style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_height(dd, 28);
    lv_obj_set_style_pad_ver(dd, 5, 0);
    lv_obj_set_style_radius(dd, 3, 0);
    lv_obj_set_style_bg_color(dd, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(dd, UI_COL_BORDER, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
}

void ui_ports_open(uint32_t ip)
{
    s_preset_ip = ip;
    s_from_network = true;
    ui_go(UI_SCR_PORTS);
}

lv_obj_t *ui_ports_create(void)
{
    if (!W) W = (scratch_t *)heap_caps_calloc_prefer(1, sizeof(scratch_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Port scan", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    if (s_from_network) {
        ui_set_back_target(UI_SCR_NETWORK);
        s_from_network = false;
    }
    if (s_preset_ip) {
        s_target = s_preset_ip;
        bool in_lan = false;
        int hn = wifi_mgr_lan_hosts(s_hosts, LAN_MAX_HOSTS);
        for (int i = 0; i < hn; i++) in_lan |= s_hosts[i].ip == s_preset_ip;
        if (!in_lan) s_custom_ip = s_preset_ip;
    }

    S.target_dd = lv_dropdown_create(content);
    lv_obj_set_width(S.target_dd, 148);
    style_dropdown(S.target_dd);
    lv_obj_align(S.target_dd, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(S.target_dd, on_target, LV_EVENT_VALUE_CHANGED, nullptr);

    S.profile_dd = lv_dropdown_create(content);
    lv_dropdown_set_options(S.profile_dd, "Top ports\n1-1024");
    lv_dropdown_set_selected(S.profile_dd, s_profile);
    lv_obj_set_width(S.profile_dd, 88);
    style_dropdown(S.profile_dd);
    lv_obj_align(S.profile_dd, LV_ALIGN_TOP_LEFT, 152, 0);
    lv_obj_add_event_cb(S.profile_dd, on_profile, LV_EVENT_VALUE_CHANGED, nullptr);

    S.btn = ui_button(content, "Scan", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 64, 28);
    lv_obj_set_style_pad_all(S.btn, 0, 0);
    lv_obj_align(S.btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.btn, on_button, LV_EVENT_CLICKED, nullptr);

    S.status = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(S.status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.status, LV_PCT(100));
    lv_obj_align(S.status, LV_ALIGN_TOP_LEFT, 0, 33);
    S.bar = ui_bar(content, LV_PCT(100), 2);
    lv_obj_align(S.bar, LV_ALIGN_TOP_LEFT, 0, 50);

    S.list = lv_obj_create(content);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_size(S.list, LV_PCT(100), UI_CONTENT_H - 12 - 56);
    lv_obj_align(S.list, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 3, 0);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_ACTIVE);

    S.seen_lan_gen = wifi_mgr_lan_gen();
    rebuild_targets();
    S.seen_scan_gen = portscan_gen();
    rebuild_results();
    S.shown_running = true; /* force the button label on the first poll */

    if (wifi_mgr_lan_host_count() == 0 && wifi_mgr_state() == WIFI_ST_CONNECTED) wifi_mgr_lan_scan_start();
    if (s_preset_ip) {
        s_preset_ip = 0;
        start_scan();
    }

    lv_timer_t *t = ui_screen_timer(content, poll, 400, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}
