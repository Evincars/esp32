#include <algorithm>
#include <cmath>
#include <cstring>

#include "app_settings.h"
#include "esp_timer.h"
#include "ui.h"
#include "wifi_mgr.h"

#define CH_MIN 1
#define CH_MAX 13
#define X_MIN (-1.0f) /* plot range in channel units, wide enough for 40 MHz at the band edges */
#define X_MAX 15.0f
/* real 2.4 GHz signals rarely exceed -30 dBm, so the old -20 top wasted the
   upper third of the plot; tightened to the band that is actually used */
#define RSSI_FLOOR (-95)
#define RSSI_TOP (-30)

#define INFO_H 20
#define GRAPH_H 166
#define STRIP_H 22
#define PLOT_PAD_L 24
#define PLOT_PAD_B 14
#define PLOT_PAD_T 4
#define HILL_SEGMENTS 12
#define LABELED_APS 10


static struct {
    lv_obj_t *graph;
    lv_obj_t *info;
    lv_obj_t *list;
    lv_obj_t *strip_cells[CH_MAX];
    lv_obj_t *strip_counts[CH_MAX];
    wifi_ap_record_t aps[WIFI_MAX_APS];
    float from_rssi[WIFI_MAX_APS]; /* animated: drawn = from + (to - from) * t */
    int ap_count;
    int32_t t; /* 0..1000 */
    uint32_t seen_gen;
    int64_t last_scan_us;
    uint8_t highlight[6];
    bool has_highlight;
} S;

/* Curves get tones mixed from the palette's accent and good colors, so the graph
   stays within the active theme while neighbours remain distinguishable. */
static lv_color_t ap_color(const wifi_ap_record_t *ap)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ ap->bssid[i]) * 16777619u;
    switch (h % 6) {
    case 0: return UI_COL_ACCENT;
    case 1: return UI_COL_GOOD;
    case 2: return lv_color_mix(UI_COL_ACCENT, UI_COL_GOOD, LV_OPA_50);
    case 3: return lv_color_mix(UI_COL_ACCENT, UI_COL_TEXT, LV_OPA_60);
    case 4: return lv_color_mix(UI_COL_GOOD, UI_COL_TEXT, LV_OPA_60);
    default: return lv_color_mix(UI_COL_TEXT, UI_COL_ACCENT, LV_OPA_70);
    }
}

static float drawn_rssi(int i)
{
    return S.from_rssi[i] + (S.aps[i].rssi - S.from_rssi[i]) * (S.t / 1000.0f);
}

static float half_width(const wifi_ap_record_t *ap)
{
    /* channels are 5 MHz apart: 20 MHz spans +-2 channels, 40 MHz +-4 */
    return wifi_ap_bandwidth_mhz(ap) / 10.0f;
}

/* ---------------- drawing ---------------- */

typedef struct {
    int32_t x0, y0, w, h; /* plot area in screen coords */
} plot_t;

static int32_t px_x(const plot_t &p, float ch)
{
    return p.x0 + (int32_t)lroundf((ch - X_MIN) / (X_MAX - X_MIN) * p.w);
}

static int32_t px_y(const plot_t &p, float rssi)
{
    float f = (rssi - RSSI_FLOOR) / (float)(RSSI_TOP - RSSI_FLOOR);
    f = std::clamp(f, 0.0f, 1.0f);
    return p.y0 + p.h - (int32_t)lroundf(f * p.h);
}

static float hill_shape(float t)
{
    /* flat-topped bell: steep flanks like a real 802.11 spectral mask */
    return std::min(1.0f, 1.6f * sinf((float)M_PI * t));
}

static void draw_grid(lv_layer_t *layer, const plot_t &p)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = UI_COL_BORDER;
    ld.width = 1;
    ld.dash_width = 2;
    ld.dash_gap = 3;

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = &lv_font_montserrat_10;
    td.color = UI_COL_TEXT_DIM;

    char buf[8];
    for (int r = -40; r >= -90; r -= 10) {
        int32_t y = px_y(p, r);
        ld.p1.x = p.x0;
        ld.p1.y = y;
        ld.p2.x = p.x0 + p.w;
        ld.p2.y = y;
        lv_draw_line(layer, &ld);
        lv_snprintf(buf, sizeof(buf), "%d", r);
        lv_area_t a = {p.x0 - PLOT_PAD_L, y - 6, p.x0 - 3, y + 6};
        td.align = LV_TEXT_ALIGN_RIGHT;
        td.text = buf;
        lv_draw_label(layer, &td, &a);
    }

    td.align = LV_TEXT_ALIGN_CENTER;
    for (int ch = CH_MIN; ch <= CH_MAX; ch++) {
        int32_t x = px_x(p, ch);
        bool major = ch == 1 || ch == 6 || ch == 11;
        ld.color = major ? UI_COL_TEXT_DIM : UI_COL_BORDER;
        ld.dash_width = 0;
        ld.p1.x = x;
        ld.p1.y = p.y0 + p.h;
        ld.p2.x = x;
        ld.p2.y = p.y0 + p.h + 3;
        lv_draw_line(layer, &ld);

        lv_snprintf(buf, sizeof(buf), "%d", ch);
        lv_area_t a = {x - 8, p.y0 + p.h + 2, x + 8, p.y0 + p.h + 14};
        td.color = major ? UI_COL_TEXT : UI_COL_TEXT_DIM;
        td.text = buf;
        lv_draw_label(layer, &td, &a);
    }

    ld.color = UI_COL_TEXT_DIM;
    ld.dash_width = 0;
    ld.p1.x = p.x0;
    ld.p1.y = p.y0 + p.h;
    ld.p2.x = p.x0 + p.w;
    ld.p2.y = p.y0 + p.h;
    lv_draw_line(layer, &ld);
}

static void draw_hill(lv_layer_t *layer, const plot_t &p, int i, bool dimmed, bool emphasized)
{
    const wifi_ap_record_t *ap = &S.aps[i];
    float center = wifi_ap_center_channel(ap);
    float hw = half_width(ap);
    float rssi = drawn_rssi(i);
    lv_color_t col = ap_color(ap);

    int32_t base = p.y0 + p.h;
    int32_t peak = px_y(p, rssi);
    lv_point_precise_t pts[HILL_SEGMENTS + 1];
    for (int s = 0; s <= HILL_SEGMENTS; s++) {
        float t = (float)s / HILL_SEGMENTS;
        pts[s].x = px_x(p, center - hw + 2 * hw * t);
        pts[s].y = base - (int32_t)lroundf((base - peak) * hill_shape(t));
    }

    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.bg_color = col;
    tri.bg_opa = dimmed ? LV_OPA_10 : emphasized ? LV_OPA_40 : LV_OPA_20;
    for (int s = 0; s < HILL_SEGMENTS; s++) {
        /* quad (s, s+1, base) split into two triangles */
        tri.p[0] = pts[s];
        tri.p[1] = pts[s + 1];
        tri.p[2] = {pts[s + 1].x, base};
        lv_draw_triangle(layer, &tri);
        tri.p[0] = pts[s];
        tri.p[1] = {pts[s + 1].x, base};
        tri.p[2] = {pts[s].x, base};
        lv_draw_triangle(layer, &tri);
    }

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.width = emphasized ? 3 : 2;
    ld.opa = dimmed ? LV_OPA_40 : LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int s = 0; s < HILL_SEGMENTS; s++) {
        ld.p1 = pts[s];
        ld.p2 = pts[s + 1];
        lv_draw_line(layer, &ld);
    }
}

static void draw_label(lv_layer_t *layer, const plot_t &p, int i, bool dimmed)
{
    const wifi_ap_record_t *ap = &S.aps[i];
    char name[14];
    if (ap->ssid[0]) {
        strlcpy(name, (const char *)ap->ssid, sizeof(name));
        if (strlen((const char *)ap->ssid) >= sizeof(name)) strcpy(name + sizeof(name) - 3, "..");
    } else {
        strcpy(name, "(hidden)");
    }

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font = &lv_font_montserrat_10;
    td.color = ap_color(ap);
    td.opa = dimmed ? LV_OPA_40 : LV_OPA_COVER;
    td.align = LV_TEXT_ALIGN_CENTER;
    td.text = name;

    int32_t cx = px_x(p, wifi_ap_center_channel(ap));
    cx = std::clamp(cx, p.x0 + 34, p.x0 + p.w - 34);
    int32_t y = std::max(px_y(p, drawn_rssi(i)) - 13, p.y0 - 2);
    lv_area_t a = {cx - 40, y, cx + 40, y + 12};
    lv_draw_label(layer, &td, &a);
}

static plot_t plot_area(lv_obj_t *obj)
{
    lv_area_t c;
    lv_obj_get_content_coords(obj, &c);
    plot_t p;
    p.x0 = c.x1 + PLOT_PAD_L;
    p.y0 = c.y1 + PLOT_PAD_T;
    p.w = lv_area_get_width(&c) - PLOT_PAD_L - 4;
    p.h = lv_area_get_height(&c) - PLOT_PAD_T - PLOT_PAD_B;
    return p;
}

static void on_graph_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    plot_t p = plot_area(obj);

    draw_grid(layer, p);

    /* draw weak first so strong networks sit on top */
    int order[WIFI_MAX_APS];
    for (int i = 0; i < S.ap_count; i++) order[i] = i;
    std::sort(order, order + S.ap_count, [](int a, int b) { return S.aps[a].rssi < S.aps[b].rssi; });

    int hl = -1;
    for (int k = 0; k < S.ap_count; k++) {
        int i = order[k];
        bool is_hl = S.has_highlight && memcmp(S.aps[i].bssid, S.highlight, 6) == 0;
        if (is_hl) {
            hl = i;
            continue;
        }
        draw_hill(layer, p, i, S.has_highlight, false);
    }
    if (hl >= 0) draw_hill(layer, p, hl, false, true);

    /* label only the strongest few to avoid a word cloud */
    for (int k = S.ap_count - 1, n = 0; k >= 0 && n < LABELED_APS; k--, n++) {
        int i = order[k];
        if (i != hl) draw_label(layer, p, i, S.has_highlight);
    }
    if (hl >= 0) draw_label(layer, p, hl, false);
}

/* ---------------- interaction ---------------- */

static void update_info(void);

static void on_graph_clicked(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    plot_t p = plot_area(obj);

    /* pick the hill whose outline at this x is closest to the tap */
    int best = -1;
    int32_t best_d = 44;
    for (int i = 0; i < S.ap_count; i++) {
        const wifi_ap_record_t *ap = &S.aps[i];
        float center = wifi_ap_center_channel(ap), hw = half_width(ap);
        int32_t x_lo = px_x(p, center - hw), x_hi = px_x(p, center + hw);
        if (pt.x <= x_lo || pt.x >= x_hi) continue;
        float t = (float)(pt.x - x_lo) / (x_hi - x_lo);
        int32_t base = p.y0 + p.h;
        int32_t y = base - (int32_t)((base - px_y(p, ap->rssi)) * hill_shape(t));
        int32_t d = abs(y - pt.y);
        if (pt.y > y) d /= 3; /* tapping inside a hill counts as near it */
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    if (best >= 0 && !(S.has_highlight && memcmp(S.highlight, S.aps[best].bssid, 6) == 0)) {
        memcpy(S.highlight, S.aps[best].bssid, 6);
        S.has_highlight = true;
    } else {
        S.has_highlight = false;
    }
    update_info();
    lv_obj_invalidate(S.graph);
}

/* ---------------- congestion ---------------- */

static void update_strip_and_info(int *best_ch)
{
    float score[CH_MAX + 1] = {};
    int count[CH_MAX + 1] = {};
    for (int i = 0; i < S.ap_count; i++) {
        const wifi_ap_record_t *ap = &S.aps[i];
        if (ap->primary >= CH_MIN && ap->primary <= CH_MAX) count[ap->primary]++;
        float center = wifi_ap_center_channel(ap);
        float reach = half_width(ap) + 2.0f; /* overlap with a 20 MHz channel at c */
        float weight = std::max(0, ap->rssi + 95) / 10.0f + 0.5f;
        for (int c = CH_MIN; c <= CH_MAX; c++) {
            float d = fabsf(c - center);
            if (d < reach) score[c] += weight * (1.0f - d / reach);
        }
    }

    float max_score = 0.001f;
    for (int c = CH_MIN; c <= CH_MAX; c++) max_score = std::max(max_score, score[c]);
    for (int c = CH_MIN; c <= CH_MAX; c++) {
        float f = score[c] / max_score;
        lv_color_t col = f < 0.33f ? UI_COL_GOOD : f < 0.66f ? UI_COL_ACCENT : UI_COL_BAD;
        lv_obj_set_style_bg_color(S.strip_cells[c - 1], col, 0);
        lv_obj_set_style_bg_opa(S.strip_cells[c - 1], (lv_opa_t)(40 + f * 150), 0);
        if (count[c]) lv_label_set_text_fmt(S.strip_counts[c - 1], "%d", count[c]);
        else lv_label_set_text(S.strip_counts[c - 1], "");
    }

    /* recommend among the non-overlapping trio */
    const int candidates[] = {1, 6, 11};
    *best_ch = 1;
    for (int c : candidates) {
        if (score[c] < score[*best_ch]) *best_ch = c;
    }
}

static void update_info(void)
{
    if (S.has_highlight) {
        for (int i = 0; i < S.ap_count; i++) {
            const wifi_ap_record_t *ap = &S.aps[i];
            if (memcmp(ap->bssid, S.highlight, 6) != 0) continue;
            lv_label_set_text_fmt(S.info, "%s  -  ch %d  %d MHz  %d dBm", ap->ssid[0] ? (const char *)ap->ssid : "(hidden)",
                                  ap->primary, wifi_ap_bandwidth_mhz(ap), ap->rssi);
            lv_obj_set_style_text_color(S.info, ap_color(ap), 0);
            return;
        }
        S.has_highlight = false; /* AP disappeared from the latest scan */
    }
    int best;
    update_strip_and_info(&best);
    if (S.ap_count) {
        lv_label_set_text_fmt(S.info, "%d APs   best channel: %d   (tap a curve)", S.ap_count, best);
    } else {
        lv_label_set_text(S.info, "Scanning the 2.4 GHz band...");
    }
    lv_obj_set_style_text_color(S.info, UI_COL_TEXT_DIM, 0);
}

/* ---------------- detail table ---------------- */

static void update_info(void);

/* Tapping a table row selects that AP on the graph - much easier than hitting a
   thin overlapping curve by touch. */
static void on_list_row(lv_event_t *e)
{
    int oi = (int)(intptr_t)lv_event_get_user_data(e);
    if (oi >= S.ap_count) return;
    if (S.has_highlight && memcmp(S.highlight, S.aps[oi].bssid, 6) == 0) {
        S.has_highlight = false;
    } else {
        memcpy(S.highlight, S.aps[oi].bssid, 6);
        S.has_highlight = true;
    }
    update_info();
    lv_obj_invalidate(S.graph);
}

/* A scrollable list of every AP below the graph, strongest first, so the screen
   shows all the detail the scan gathered (the graph only labels the loudest few). */
static void rebuild_ap_list(void)
{
    if (!S.list) return;
    int order[WIFI_MAX_APS];
    for (int i = 0; i < S.ap_count; i++) order[i] = i;
    std::sort(order, order + S.ap_count, [](int a, int b) { return S.aps[a].rssi > S.aps[b].rssi; });

    lv_obj_clean(S.list);
    for (int k = 0; k < S.ap_count; k++) {
        int oi = order[k];
        const wifi_ap_record_t *ap = &S.aps[oi];
        lv_obj_t *row = lv_button_create(S.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 22);
        lv_obj_set_style_bg_color(row, UI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_add_event_cb(row, on_list_row, LV_EVENT_CLICKED, (void *)(intptr_t)oi);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, ap_color(ap), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = ui_label(row, &lv_font_montserrat_12, ap->ssid[0] ? UI_COL_TEXT : UI_COL_TEXT_DIM,
                                  ap->ssid[0] ? (const char *)ap->ssid : "(hidden)");
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 150);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *meta = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(meta, "ch%d %dMHz", ap->primary, wifi_ap_bandwidth_mhz(ap));
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -46, 0);

        lv_obj_t *rssi = ui_label(row, &lv_font_montserrat_12, ui_rssi_color(ap->rssi), "");
        lv_label_set_text_fmt(rssi, "%d", ap->rssi);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

/* ---------------- data / animation ---------------- */

static void t_exec(void *, int32_t v)
{
    S.t = v;
    lv_obj_invalidate(S.graph);
}

static void load_new_scan(void)
{
    static wifi_ap_record_t fresh[WIFI_MAX_APS]; /* too big for the LVGL task stack */
    int n = wifi_mgr_get_aps(fresh, WIFI_MAX_APS);

    /* start each hill from where it is currently drawn, new ones grow from the floor */
    float from[WIFI_MAX_APS];
    for (int i = 0; i < n; i++) {
        from[i] = RSSI_FLOOR;
        for (int j = 0; j < S.ap_count; j++) {
            if (memcmp(fresh[i].bssid, S.aps[j].bssid, 6) == 0) {
                from[i] = drawn_rssi(j);
                break;
            }
        }
    }
    memcpy(S.aps, fresh, n * sizeof(wifi_ap_record_t));
    memcpy(S.from_rssi, from, n * sizeof(float));
    S.ap_count = n;
    S.t = 0;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &S);
    lv_anim_set_exec_cb(&a, t_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 650);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    update_info();
    rebuild_ap_list();
}

static void poll(lv_timer_t *)
{
    uint32_t gen = wifi_mgr_scan_gen();
    if (gen != S.seen_gen) {
        S.seen_gen = gen;
        load_new_scan();
    }
    int64_t now = esp_timer_get_time();
    if (!wifi_mgr_scanning() && now - S.last_scan_us > (int64_t)g_settings.scan_interval_s * 1000000) {
        S.last_scan_us = now;
        wifi_mgr_scan_start();
    }
}

static void on_delete(lv_event_t *)
{
    lv_anim_delete(&S, t_exec);
}

lv_obj_t *ui_channels_create(void)
{
    lv_anim_delete(&S, t_exec);
    memset(&S, 0, sizeof(S));

    lv_obj_t *content = ui_screen_create("Channel graph", true);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_add_event_cb(content, on_delete, LV_EVENT_DELETE, nullptr);

    S.info = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(S.info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.info, LV_PCT(100));
    lv_obj_align(S.info, LV_ALIGN_TOP_LEFT, 2, 2);

    S.graph = ui_card(content);
    lv_obj_set_size(S.graph, LV_PCT(100), GRAPH_H);
    lv_obj_align(S.graph, LV_ALIGN_TOP_LEFT, 0, INFO_H);
    lv_obj_set_style_pad_all(S.graph, 4, 0);
    lv_obj_add_flag(S.graph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(S.graph, on_graph_draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(S.graph, on_graph_clicked, LV_EVENT_CLICKED, nullptr);

    /* congestion strip: one cell per channel, aligned with the plot's x axis */
    lv_obj_t *strip = lv_obj_create(content);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_PCT(100), STRIP_H);
    lv_obj_align(strip, LV_ALIGN_TOP_LEFT, 0, INFO_H + GRAPH_H + 4);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lbl = ui_label(strip, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "load");
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    /* mirror plot_area(): card pad 4 + border 1, then PLOT_PAD_L */
    const int card_w = 320 - 12;
    plot_t p = {5 + PLOT_PAD_L, 0, card_w - 10 - PLOT_PAD_L - 4, 0};
    for (int c = CH_MIN; c <= CH_MAX; c++) {
        int32_t x = px_x(p, c);
        int32_t cw = px_x(p, c + 1) - x - 2;
        lv_obj_t *cell = lv_obj_create(strip);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cw, STRIP_H);
        lv_obj_set_pos(cell, x - cw / 2, 0);
        lv_obj_set_style_radius(cell, 2, 0);
        lv_obj_set_style_bg_color(cell, UI_COL_BORDER, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        S.strip_cells[c - 1] = cell;
        S.strip_counts[c - 1] = ui_label(cell, &lv_font_montserrat_10, UI_COL_TEXT, "");
        lv_obj_center(S.strip_counts[c - 1]);
    }

    /* full network table below the graph; scrolls with the page */
    int list_y = INFO_H + GRAPH_H + 4 + STRIP_H + 8;
    lv_obj_t *cap = ui_caption(content, "NETWORKS");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 2, list_y);
    S.list = lv_obj_create(content);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_width(S.list, LV_PCT(100));
    lv_obj_set_height(S.list, LV_SIZE_CONTENT);
    lv_obj_align(S.list, LV_ALIGN_TOP_LEFT, 0, list_y + 16);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 3, 0);
    lv_obj_remove_flag(S.list, LV_OBJ_FLAG_SCROLLABLE);

    S.seen_gen = wifi_mgr_scan_gen();
    load_new_scan();
    S.last_scan_us = esp_timer_get_time();
    wifi_mgr_scan_start();
    ui_screen_timer(content, poll, 250, nullptr);
    return lv_obj_get_screen(content);
}
