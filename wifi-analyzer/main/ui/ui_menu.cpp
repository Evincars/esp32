#include <cstdarg>
#include <cstdint>
#include <ctime>

#include "app_settings.h"
#include "portscan.h"
#include "rogue_ap.h"
#include "sniffer.h"
#include "sysmon.h"
#include "time_sync.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_attack.h"
#include "wifi_csi.h"
#include "wifi_ftm.h"
#include "wifi_mgr.h"

#define COLS 2
#define GAP 4
#define TILE_H 46

typedef struct {
    const char *title;
    ui_screen_id_t target;
} tile_def_t;

static const tile_def_t TILES[] = {
    {"Scanner", UI_SCR_SCANNER},    {"Channels", UI_SCR_CHANNELS}, {"Network", UI_SCR_NETWORK},
    {"Port scan", UI_SCR_PORTS},    {"Sniffer", UI_SCR_SNIFFER},   {"Deauth", UI_SCR_DEAUTH},
    {"Beacon spam", UI_SCR_BEACON}, {"Rogue AP", UI_SCR_ROGUE},    {"CSI Radar", UI_SCR_CSI},
    {"Locate", UI_SCR_LOCATE},      {"Web server", UI_SCR_WEB},    {"System", UI_SCR_SYSTEM},
    {"Clock", UI_SCR_CLOCK},        {"Settings", UI_SCR_SETTINGS}, {"About", UI_SCR_ABOUT},
};
#define TILE_COUNT (int)(sizeof(TILES) / sizeof(TILES[0]))

static lv_obj_t *s_values[TILE_COUNT];

static void on_tile_clicked(lv_event_t *e)
{
    ui_go((ui_screen_id_t)(intptr_t)lv_event_get_user_data(e));
}

/* Live one-liner on the second line of each tile. */
static void set_value(int i, lv_color_t col, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_value(int i, lv_color_t col, const char *fmt, ...)
{
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    lv_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(s_values[i], buf);
    lv_obj_set_style_text_color(s_values[i], col, 0);
}

static void update(lv_timer_t *)
{
    lv_color_t dim = UI_COL_TEXT_DIM;
    for (int i = 0; i < TILE_COUNT; i++) {
        switch (TILES[i].target) {
        case UI_SCR_SCANNER: {
            int n = wifi_mgr_ap_count();
            if (n) set_value(i, dim, "%d APs", n);
            else set_value(i, dim, "%s", wifi_mgr_scanning() ? "scanning" : "-");
            break;
        }
        case UI_SCR_CHANNELS:
            set_value(i, dim, "2.4 GHz");
            break;
        case UI_SCR_NETWORK: {
            wifi_conn_info_t info;
            if (wifi_mgr_conn_info(&info)) set_value(i, UI_COL_GOOD, IPSTR, IP2STR(&info.ip.ip));
            else if (wifi_mgr_state() == WIFI_ST_CONNECTING) set_value(i, UI_COL_ACCENT, "joining");
            else set_value(i, dim, "offline");
            break;
        }
        case UI_SCR_PORTS: {
            ps_status_t st;
            portscan_status(&st);
            if (st.running) set_value(i, UI_COL_ACCENT, "%d/%d", st.ports_done, st.ports_total);
            else if (st.hosts_total) set_value(i, dim, "%d open", st.open_count);
            else set_value(i, dim, "idle");
            break;
        }
        case UI_SCR_SNIFFER: {
            sniff_stats_t st;
            sniffer_stats(&st);
            if (sniffer_running()) set_value(i, UI_COL_ACCENT, "%lu pkts", (unsigned long)st.total);
            else set_value(i, dim, "stopped");
            break;
        }
        case UI_SCR_DEAUTH: {
            attack_status_t st;
            wifi_attack_deauth_status(&st);
            if (st.running) set_value(i, UI_COL_BAD, "%lu/s", (unsigned long)st.pps);
            else set_value(i, dim, "idle");
            break;
        }
        case UI_SCR_BEACON: {
            attack_status_t st;
            wifi_attack_beacon_status(&st);
            if (st.running) set_value(i, UI_COL_ACCENT, "%lu/s", (unsigned long)st.pps);
            else set_value(i, dim, "idle");
            break;
        }
        case UI_SCR_ROGUE: {
            rogue_status_t st;
            rogue_ap_status(&st);
            if (st.running) set_value(i, UI_COL_BAD, "%d cli %d cap", st.clients, st.cred_count);
            else set_value(i, dim, "off");
            break;
        }
        case UI_SCR_CSI: {
            csi_status_t st;
            wifi_csi_status(&st);
            if (st.running)
                set_value(i, st.presence ? UI_COL_BAD : UI_COL_GOOD, "%s %d",
                          st.presence ? "motion" : "clear", st.motion);
            else set_value(i, dim, "off");
            break;
        }
        case UI_SCR_LOCATE: {
            ftm_status_t st;
            wifi_ftm_status(&st);
            if (st.running && st.state == FTM_OK) set_value(i, UI_COL_GOOD, "%.1f m", st.dist_cm / 100.0f);
            else if (st.running) set_value(i, UI_COL_ACCENT, "measuring");
            else set_value(i, dim, "idle");
            break;
        }
        case UI_SCR_WEB:
            if (web_server_running()) set_value(i, UI_COL_GOOD, "on :80");
            else set_value(i, dim, "off");
            break;
        case UI_SCR_SYSTEM: {
            sysmon_t s;
            sysmon_get(&s);
            set_value(i, dim, "%d%%  %d\xC2\xB0""C", (int)((s.cpu[0] + s.cpu[1]) / 2), (int)s.temp_c);
            break;
        }
        case UI_SCR_CLOCK:
            if (time_sync_valid()) {
                time_t now = time(nullptr);
                struct tm tm;
                localtime_r(&now, &tm);
                set_value(i, dim, "%02d:%02d", tm.tm_hour, tm.tm_min);
            } else {
                set_value(i, dim, "not synced");
            }
            break;
        case UI_SCR_SETTINGS:
            set_value(i, dim, "%s", g_pal->name);
            break;
        default:
            set_value(i, dim, "Evincars");
            break;
        }
    }
}

lv_obj_t *ui_menu_create(void)
{
    lv_obj_t *content = ui_screen_create("WiFi Toolkit", false);
    /* vertical scrolling grid of tiles */
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(content, GAP, 0);
    lv_obj_set_style_pad_column(content, GAP, 0);

    const int tile_w = (320 - 2 * 6 - (COLS - 1) * GAP) / COLS;

    for (int i = 0; i < TILE_COUNT; i++) {
        lv_obj_t *tile = lv_button_create(content);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, tile_w, TILE_H);
        lv_obj_set_style_bg_color(tile, UI_COL_CARD, 0);
        lv_obj_set_style_bg_color(tile, UI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, UI_COL_BORDER, 0);
        lv_obj_set_style_border_color(tile, UI_COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_radius(tile, UI_RADIUS, 0);
        lv_obj_set_style_pad_hor(tile, 8, 0);
        lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)TILES[i].target);

        lv_obj_t *title = ui_label(tile, &lv_font_montserrat_12, UI_COL_TEXT, TILES[i].title);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 5);
        /* value on its own line below the title so long text (e.g. an IP) never overlaps */
        s_values[i] = ui_label(tile, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_long_mode(s_values[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_values[i], tile_w - 16);
        lv_obj_align(s_values[i], LV_ALIGN_BOTTOM_LEFT, 0, -5);
    }

    lv_timer_t *t = ui_screen_timer(content, update, 1000, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
