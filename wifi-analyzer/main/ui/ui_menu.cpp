#include <cstdarg>
#include <cstdint>
#include <ctime>

#include "app_settings.h"
#include "portscan.h"
#include "sniffer.h"
#include "sysmon.h"
#include "time_sync.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_mgr.h"

#define COLS 2
#define GAP 4

typedef struct {
    const char *title;
    ui_screen_id_t target;
} tile_def_t;

static const tile_def_t TILES[] = {
    {"Scanner", UI_SCR_SCANNER}, {"Channels", UI_SCR_CHANNELS}, {"Network", UI_SCR_NETWORK},
    {"Port scan", UI_SCR_PORTS}, {"Sniffer", UI_SCR_SNIFFER},   {"Web server", UI_SCR_WEB},
    {"System", UI_SCR_SYSTEM},   {"Clock", UI_SCR_CLOCK},       {"Settings", UI_SCR_SETTINGS},
    {"About", UI_SCR_ABOUT},
};
#define TILE_COUNT (int)(sizeof(TILES) / sizeof(TILES[0]))

static lv_obj_t *s_values[TILE_COUNT];

static void on_tile_clicked(lv_event_t *e)
{
    ui_go((ui_screen_id_t)(intptr_t)lv_event_get_user_data(e));
}

/* Live one-liner on the right side of each tile. */
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
        case UI_SCR_WEB:
            if (web_server_running()) set_value(i, UI_COL_GOOD, "on :80");
            else set_value(i, dim, "off");
            break;
        case UI_SCR_SYSTEM: {
            sysmon_t s;
            sysmon_get(&s);
            set_value(i, dim, "%d%%  %d°C", (int)((s.cpu[0] + s.cpu[1]) / 2), (int)s.temp_c);
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
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    const int rows = (TILE_COUNT + COLS - 1) / COLS;
    const int inner_w = 320 - 12, inner_h = UI_CONTENT_H - 12;
    const int tile_w = (inner_w - (COLS - 1) * GAP) / COLS;
    const int tile_h = (inner_h - (rows - 1) * GAP) / rows;

    for (int i = 0; i < TILE_COUNT; i++) {
        lv_obj_t *tile = lv_button_create(content);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_set_pos(tile, (i % COLS) * (tile_w + GAP), (i / COLS) * (tile_h + GAP));
        lv_obj_set_style_bg_color(tile, UI_COL_CARD, 0);
        lv_obj_set_style_bg_color(tile, UI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, UI_COL_BORDER, 0);
        lv_obj_set_style_border_color(tile, UI_COL_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_radius(tile, UI_RADIUS, 0);
        lv_obj_set_style_pad_hor(tile, 10, 0);
        lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)TILES[i].target);

        lv_obj_t *title = ui_label(tile, &lv_font_montserrat_14, UI_COL_TEXT, TILES[i].title);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = ui_label(tile, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_timer_t *t = ui_screen_timer(content, update, 1000, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
