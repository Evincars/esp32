#include <cstdio>
#include <cstring>

#include "ui.h"
#include "wifi_attack.h"

static struct {
    lv_obj_t *slider, *count_lbl, *state, *btn, *btn_lbl, *v_frames, *v_pps, *v_ch;
    int count;
} S;

static void update(lv_timer_t *)
{
    attack_status_t st;
    wifi_attack_beacon_status(&st);
    bool on = st.running;
    lv_label_set_text(S.state, on ? "Broadcasting" : "Idle");
    lv_obj_set_style_text_color(S.state, on ? UI_COL_ACCENT : UI_COL_TEXT_DIM, 0);
    lv_label_set_text(S.btn_lbl, on ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);
    lv_label_set_text_fmt(S.v_frames, "%lu", (unsigned long)st.frames);
    lv_label_set_text_fmt(S.v_pps, "%lu/s", (unsigned long)st.pps);
    lv_label_set_text_fmt(S.v_ch, "%d", st.channel);
    if (on) lv_obj_add_state(S.slider, LV_STATE_DISABLED);
    else lv_obj_remove_state(S.slider, LV_STATE_DISABLED);
}

static void on_slider(lv_event_t *e)
{
    S.count = lv_slider_get_value(lv_event_get_target_obj(e));
    lv_label_set_text_fmt(S.count_lbl, "%d fake SSIDs", S.count);
}

static void do_start(void)
{
    if (!wifi_attack_beacon_start("FreeWiFi", S.count)) ui_toast("Could not start", UI_COL_BAD);
    update(nullptr);
}

static void on_toggle(lv_event_t *)
{
    if (wifi_attack_beacon_running()) {
        wifi_attack_beacon_stop();
        update(nullptr);
    } else {
        ui_confirm("Beacon spam",
                   "Floods the air with fake access points on channels 1/6/11. For authorized "
                   "resilience testing only. Drops your own Wi-Fi link.",
                   "Start", UI_COL_BAD, do_start);
    }
}

lv_obj_t *ui_beacon_create(void)
{
    memset(&S, 0, sizeof(S));
    S.count = 24;
    lv_obj_t *content = ui_screen_create("Beacon spam", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = ui_caption(content, "NUMBER OF FAKE NETWORKS");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    S.slider = lv_slider_create(content);
    lv_obj_set_width(S.slider, LV_PCT(100));
    lv_slider_set_range(S.slider, 1, ATTACK_MAX_BEACONS);
    lv_slider_set_value(S.slider, S.count, LV_ANIM_OFF);
    lv_obj_align(S.slider, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_add_event_cb(S.slider, on_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    S.count_lbl = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_label_set_text_fmt(S.count_lbl, "%d fake SSIDs", S.count);
    lv_obj_align(S.count_lbl, LV_ALIGN_TOP_LEFT, 0, 40);

    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 66);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    S.state = ui_kv_row(card, "State");
    S.v_frames = ui_kv_row(card, "Beacons sent");
    S.v_pps = ui_kv_row(card, "Rate");
    S.v_ch = ui_kv_row(card, "Channel");

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 110, 30);
    lv_obj_align(S.btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    lv_timer_t *t = ui_screen_timer(content, update, 500, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
