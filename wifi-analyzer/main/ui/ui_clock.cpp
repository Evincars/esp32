#include <cmath>
#include <cstring>
#include <ctime>

#include "app_settings.h"
#include "esp_timer.h"
#include "sysmon.h"
#include "time_sync.h"
#include "ui.h"
#include "wifi_mgr.h"

#define RING_SIZE 220
#define R_OUT 106
#define R_MINOR 99
#define R_MAJOR 93

static struct {
    lv_obj_t *ring;
    lv_obj_t *time, *date, *weekday;
    lv_obj_t *zone, *sync, *power, *temp;
    int second; /* 0..59, drives the ring */
    int shown;  /* last second rendered */
} S;

static float s_cos[60], s_sin[60];

static void on_ring_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float cx = a.x1 + lv_area_get_width(&a) / 2.0f, cy = a.y1 + lv_area_get_height(&a) / 2.0f;

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.round_start = 0;
    ld.round_end = 0;
    for (int i = 0; i < 60; i++) {
        bool major = i % 5 == 0;
        bool lit = i <= S.second;
        float r_in = major ? R_MAJOR : R_MINOR;
        ld.width = major ? 3 : 2;
        ld.color = i == S.second ? UI_COL_TEXT : lit ? UI_COL_ACCENT : UI_COL_BORDER;
        ld.opa = lit || major ? LV_OPA_COVER : LV_OPA_70;
        ld.p1.x = cx + s_sin[i] * r_in;
        ld.p1.y = cy - s_cos[i] * r_in;
        ld.p2.x = cx + s_sin[i] * R_OUT;
        ld.p2.y = cy - s_cos[i] * R_OUT;
        lv_draw_line(layer, &ld);
    }
}

static void update(lv_timer_t *)
{
    static const char *DAYS[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
    static const char *MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

    bool valid = time_sync_valid();
    if (valid) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        S.second = tm.tm_sec % 60;
        lv_label_set_text_fmt(S.time, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text(S.weekday, DAYS[tm.tm_wday]);
        lv_label_set_text_fmt(S.date, "%d %s %d", tm.tm_mday, MONTHS[tm.tm_mon], tm.tm_year + 1900);
        lv_label_set_text(S.sync, "NTP synced");
        lv_obj_set_style_text_color(S.sync, UI_COL_TEXT_DIM, 0);
    } else {
        /* no wall clock yet: keep the ring alive with the uptime seconds */
        S.second = (int)(esp_timer_get_time() / 1000000 % 60);
        lv_label_set_text(S.time, "--:--");
        lv_label_set_text(S.weekday, "");
        lv_label_set_text(S.date, wifi_mgr_state() == WIFI_ST_CONNECTED ? "syncing time..." : "connect Wi-Fi to sync");
        lv_label_set_text(S.sync, "no time");
        lv_obj_set_style_text_color(S.sync, UI_COL_ACCENT, 0);
    }

    sysmon_t s;
    sysmon_get(&s);
    if (s.on_usb) lv_label_set_text(S.power, "USB");
    else lv_label_set_text_fmt(S.power, "%d%%", s.batt_pct);
    if (s.temp_ok) lv_label_set_text_fmt(S.temp, "%d°C", (int)s.temp_c);

    if (S.second != S.shown) {
        S.shown = S.second;
        lv_obj_invalidate(S.ring);
    }
}

static void on_tap(lv_event_t *)
{
    ui_go(UI_SCR_MENU);
}

static lv_obj_t *corner(lv_obj_t *scr, lv_align_t align, int x, int y)
{
    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_obj_align(l, align, x, y);
    return l;
}

lv_obj_t *ui_clock_create(void)
{
    memset(&S, 0, sizeof(S));
    S.shown = -1;
    for (int i = 0; i < 60; i++) {
        float a = i * 6.0f * (float)M_PI / 180.0f;
        s_sin[i] = sinf(a);
        s_cos[i] = cosf(a);
    }

    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, nullptr);

    S.ring = lv_obj_create(scr);
    lv_obj_remove_style_all(S.ring);
    lv_obj_set_size(S.ring, RING_SIZE, RING_SIZE);
    lv_obj_center(S.ring);
    lv_obj_remove_flag(S.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(S.ring, on_ring_draw, LV_EVENT_DRAW_MAIN, nullptr);

    S.weekday = ui_caption(scr, "");
    lv_obj_set_style_text_letter_space(S.weekday, 3, 0);
    lv_obj_align(S.weekday, LV_ALIGN_CENTER, 0, -44);
    S.time = ui_label(scr, &lv_font_montserrat_48, UI_COL_TEXT, "--:--");
    lv_obj_align(S.time, LV_ALIGN_CENTER, 0, -4);
    S.date = ui_label(scr, &lv_font_montserrat_14, UI_COL_TEXT_DIM, "");
    lv_obj_align(S.date, LV_ALIGN_CENTER, 0, 36);

    S.zone = corner(scr, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_label_set_text(S.zone, time_sync_zone_name(g_settings.timezone));
    S.power = corner(scr, LV_ALIGN_TOP_RIGHT, -8, 6);
    S.sync = corner(scr, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    S.temp = corner(scr, LV_ALIGN_BOTTOM_RIGHT, -8, -6);

    lv_timer_t *t = ui_screen_timer(scr, update, 200, nullptr);
    update(t);
    return scr;
}
