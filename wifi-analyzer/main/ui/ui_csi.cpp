#include <cstdio>
#include <cstring>

#include "ui.h"
#include "wifi_csi.h"

static struct {
    lv_obj_t *state, *btn, *btn_lbl, *motion_lbl, *presence, *bar, *chart, *hint, *slider;
    lv_chart_series_t *ser;
} S;

static void update(lv_timer_t *)
{
    csi_status_t st;
    wifi_csi_status(&st);
    bool on = st.running;

    lv_label_set_text(S.btn_lbl, on ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);

    if (!on) {
        lv_label_set_text(S.state, "Stopped");
        lv_obj_set_style_text_color(S.state, UI_COL_TEXT_DIM, 0);
    } else if (!st.connected) {
        lv_label_set_text(S.state, "Connect to Wi-Fi");
        lv_obj_set_style_text_color(S.state, UI_COL_BAD, 0);
    } else {
        lv_label_set_text_fmt(S.state, "%d subcarriers", st.subcarriers);
        lv_obj_set_style_text_color(S.state, UI_COL_GOOD, 0);
    }

    lv_label_set_text_fmt(S.motion_lbl, "%d", st.motion);
    lv_bar_set_value(S.bar, st.motion, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(S.bar, st.presence ? UI_COL_BAD : UI_COL_GOOD, LV_PART_INDICATOR);

    lv_label_set_text(S.presence, st.presence ? "MOTION" : "clear");
    lv_obj_set_style_text_color(S.presence, st.presence ? UI_COL_BAD : UI_COL_TEXT_DIM, 0);

    for (int i = 0; i < CSI_WAVE_LEN; i++) {
        int idx = (st.wave_head + i) % CSI_WAVE_LEN;
        lv_chart_set_value_by_id(S.chart, S.ser, i, st.wave[idx]);
    }
    lv_chart_refresh(S.chart);
}

static void on_slider(lv_event_t *e)
{
    wifi_csi_set_sensitivity(lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void on_toggle(lv_event_t *)
{
    if (wifi_csi_running()) wifi_csi_stop();
    else if (!wifi_csi_start()) ui_toast("Could not start CSI", UI_COL_BAD);
    update(nullptr);
}

lv_obj_t *ui_csi_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("CSI Radar", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = ui_caption(content, "MOTION (ESP32-S3 SIMD)");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    S.motion_lbl = ui_label(content, &lv_font_montserrat_28, UI_COL_TEXT, "0");
    lv_obj_align(S.motion_lbl, LV_ALIGN_TOP_LEFT, 0, 12);
    S.presence = ui_label(content, &lv_font_montserrat_16, UI_COL_TEXT_DIM, "clear");
    lv_obj_align(S.presence, LV_ALIGN_TOP_LEFT, 60, 20);
    S.state = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_obj_align(S.state, LV_ALIGN_TOP_RIGHT, 0, 4);

    S.bar = ui_bar(content, 300, 10);
    lv_bar_set_range(S.bar, 0, 100);
    lv_obj_align(S.bar, LV_ALIGN_TOP_LEFT, 0, 50);

    S.chart = lv_chart_create(content);
    lv_obj_set_size(S.chart, LV_PCT(100), 78);
    lv_obj_align(S.chart, LV_ALIGN_TOP_LEFT, 0, 66);
    lv_chart_set_type(S.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(S.chart, CSI_WAVE_LEN);
    lv_chart_set_range(S.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(S.chart, 3, 0);
    lv_obj_set_style_bg_color(S.chart, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(S.chart, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(S.chart, 1, 0);
    lv_obj_set_style_size(S.chart, 0, 0, LV_PART_INDICATOR);
    S.ser = lv_chart_add_series(S.chart, UI_COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *scap = ui_caption(content, "SENSITIVITY");
    lv_obj_align(scap, LV_ALIGN_BOTTOM_LEFT, 0, -36);
    S.slider = lv_slider_create(content);
    lv_obj_set_width(S.slider, 180);
    lv_slider_set_range(S.slider, 0, 100);
    lv_slider_set_value(S.slider, wifi_csi_sensitivity(), LV_ANIM_OFF);
    lv_obj_align(S.slider, LV_ALIGN_BOTTOM_LEFT, 0, -14);
    lv_obj_add_event_cb(S.slider, on_slider, LV_EVENT_VALUE_CHANGED, nullptr);

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 100, 30);
    lv_obj_align(S.btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    lv_timer_t *t = ui_screen_timer(content, update, 200, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
