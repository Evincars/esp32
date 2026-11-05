#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui.h"
#include "wifi_ftm.h"
#include "wifi_mgr.h"

#define RADAR 150
#define RCX 75
#define RCY 75
#define RMAX 68
#define MAX_CM 1500 /* full-scale radar range: 15 m */

static struct {
    lv_obj_t *dd, *vector, *dot, *state, *dist, *rtt, *rssi, *btn, *btn_lbl;
    lv_point_precise_t pts[2];
    wifi_ap_record_t aps[WIFI_MAX_APS];
    int ap_count;
    uint32_t seen_gen;
    float bearing;  /* degrees, heuristic relative heading */
    int last_rssi;
} S;

static void fill_dropdown(void)
{
    S.ap_count = wifi_mgr_get_aps(S.aps, WIFI_MAX_APS);
    char opts[WIFI_MAX_APS * 40];
    int n = 0;
    opts[0] = 0;
    if (S.ap_count == 0) {
        strcpy(opts, wifi_mgr_scanning() ? "scanning..." : "no APs - run Scanner");
    } else {
        for (int i = 0; i < S.ap_count; i++) {
            const char *ss = (const char *)S.aps[i].ssid;
            n += snprintf(opts + n, sizeof(opts) - n, "%s%s (ch%d)", i ? "\n" : "",
                          ss[0] ? ss : "<hidden>", S.aps[i].primary);
        }
    }
    lv_dropdown_set_options(S.dd, opts);
}

static void draw_vector(uint32_t dist_cm)
{
    float r = (float)dist_cm / MAX_CM * RMAX;
    if (r > RMAX) r = RMAX;
    if (r < 6) r = 6;
    float a = (S.bearing - 90.0f) * 3.14159265f / 180.0f;
    S.pts[0].x = RCX;
    S.pts[0].y = RCY;
    S.pts[1].x = (lv_coord_t)(RCX + r * cosf(a));
    S.pts[1].y = (lv_coord_t)(RCY + r * sinf(a));
    lv_line_set_points(S.vector, S.pts, 2);
    lv_obj_set_pos(S.dot, (int)S.pts[1].x - 4, (int)S.pts[1].y - 4);
}

static void update(lv_timer_t *)
{
    if (!wifi_ftm_running() && wifi_mgr_scan_gen() != S.seen_gen) {
        S.seen_gen = wifi_mgr_scan_gen();
        fill_dropdown();
    }
    ftm_status_t st;
    wifi_ftm_status(&st);
    bool on = st.running;
    lv_label_set_text(S.btn_lbl, on ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);
    if (on) lv_obj_add_state(S.dd, LV_STATE_DISABLED);
    else lv_obj_remove_state(S.dd, LV_STATE_DISABLED);

    const char *txt = "Idle";
    lv_color_t col = UI_COL_TEXT_DIM;
    switch (st.state) {
    case FTM_MEASURING: txt = "Measuring..."; col = UI_COL_ACCENT; break;
    case FTM_OK: txt = "Locked"; col = UI_COL_GOOD; break;
    case FTM_FAIL:
        /* once we have a fix, a dropped burst is just "retrying", not a hard error */
        txt = st.sessions > 0 ? "Locked - retrying" : wifi_ftm_reason(st.fail_reason);
        col = st.sessions > 0 ? UI_COL_ACCENT : UI_COL_BAD;
        break;
    default: break;
    }
    lv_label_set_text(S.state, txt);
    lv_obj_set_style_text_color(S.state, col, 0);

    /* keep the last good reading on screen; a single dropped burst shouldn't blank it */
    if (st.sessions > 0) {
        lv_label_set_text_fmt(S.dist, "%.2f m", st.dist_cm / 100.0f);
        lv_label_set_text_fmt(S.rtt, "%lu ns", (unsigned long)st.rtt_ns);
    } else {
        lv_label_set_text(S.dist, "-- m");
        lv_label_set_text(S.rtt, "--");
    }
    lv_label_set_text_fmt(S.rssi, "%d dBm", st.rssi);

    /* Heuristic relative heading: with no compass/antenna array, we use the RSSI
       trend as a warmer/colder hint - if the signal fades as you move, swing the
       vector so you can home in on the AP by keeping the reading rising. */
    if (on && st.rssi != 0) {
        if (S.last_rssi && st.rssi < S.last_rssi - 1) S.bearing += 30.0f;
        if (S.bearing >= 360.0f) S.bearing -= 360.0f;
        S.last_rssi = st.rssi;
    }
    draw_vector(st.state == FTM_OK ? st.dist_cm : 400);
}

static void on_toggle(lv_event_t *)
{
    if (wifi_ftm_running()) {
        wifi_ftm_stop();
        update(nullptr);
        return;
    }
    int sel = lv_dropdown_get_selected(S.dd);
    if (sel < 0 || sel >= S.ap_count) {
        ui_toast("Pick a target AP first", UI_COL_BAD);
        return;
    }
    const wifi_ap_record_t *ap = &S.aps[sel];
    S.bearing = 0;
    S.last_rssi = 0;
    if (!wifi_ftm_start(ap->bssid, (const char *)ap->ssid, ap->primary))
        ui_toast("Could not start FTM", UI_COL_BAD);
    update(nullptr);
}

lv_obj_t *ui_locate_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Locate (FTM)", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* radar */
    lv_obj_t *radar = lv_obj_create(content);
    lv_obj_remove_style_all(radar);
    lv_obj_set_size(radar, RADAR, RADAR);
    lv_obj_align(radar, LV_ALIGN_LEFT_MID, 0, -6);
    lv_obj_remove_flag(radar, LV_OBJ_FLAG_SCROLLABLE);
    const int rings[] = {RMAX * 2, (int)(RMAX * 1.33), (int)(RMAX * 0.66)};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(radar);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, rings[i], rings[i]);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(ring, UI_COL_BORDER, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
    }
    S.vector = lv_line_create(radar);
    lv_obj_set_style_line_color(S.vector, UI_COL_ACCENT, 0);
    lv_obj_set_style_line_width(S.vector, 3, 0);
    lv_obj_set_style_line_rounded(S.vector, true, 0);
    S.dot = lv_obj_create(radar);
    lv_obj_remove_style_all(S.dot);
    lv_obj_set_size(S.dot, 8, 8);
    lv_obj_set_style_radius(S.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(S.dot, UI_COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(S.dot, LV_OPA_COVER, 0);
    lv_obj_t *me = lv_obj_create(radar);
    lv_obj_remove_style_all(me);
    lv_obj_set_size(me, 6, 6);
    lv_obj_set_pos(me, RCX - 3, RCY - 3);
    lv_obj_set_style_radius(me, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(me, UI_COL_GOOD, 0);
    lv_obj_set_style_bg_opa(me, LV_OPA_COVER, 0);

    /* right-side info column */
    int rx = RADAR + 8;
    S.dd = lv_dropdown_create(content);
    lv_obj_set_size(S.dd, 320 - 12 - rx, 30);
    lv_obj_align(S.dd, LV_ALIGN_TOP_LEFT, rx, 0);
    fill_dropdown();
    S.seen_gen = wifi_mgr_scan_gen();

    S.dist = ui_label(content, &lv_font_montserrat_28, UI_COL_TEXT, "-- m");
    lv_obj_align(S.dist, LV_ALIGN_TOP_LEFT, rx, 36);
    S.state = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "Idle");
    lv_label_set_long_mode(S.state, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(S.state, 320 - 12 - rx);
    lv_obj_align(S.state, LV_ALIGN_TOP_LEFT, rx, 68);
    S.rtt = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "--");
    lv_obj_align(S.rtt, LV_ALIGN_TOP_LEFT, rx, 88);
    S.rssi = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "-- dBm");
    lv_obj_align(S.rssi, LV_ALIGN_TOP_LEFT, rx, 106);

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 320 - 12 - rx, 30);
    lv_obj_align(S.btn, LV_ALIGN_TOP_LEFT, rx, 130);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    draw_vector(400);
    lv_timer_t *t = ui_screen_timer(content, update, 400, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
