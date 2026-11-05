#include <cstdio>
#include <cstring>

#include "ui.h"
#include "wifi_attack.h"
#include "wifi_mgr.h"

static struct {
    lv_obj_t *dd, *state, *btn, *btn_lbl, *v_frames, *v_pps, *v_ch, *warn;
    wifi_ap_record_t aps[WIFI_MAX_APS];
    int ap_count;
    uint32_t seen_gen;
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

static void update(lv_timer_t *)
{
    if (!wifi_attack_deauth_running() && wifi_mgr_scan_gen() != S.seen_gen) {
        S.seen_gen = wifi_mgr_scan_gen();
        fill_dropdown();
    }
    attack_status_t st;
    wifi_attack_deauth_status(&st);
    bool on = st.running;
    lv_label_set_text(S.state, on ? "Flooding" : "Idle");
    lv_obj_set_style_text_color(S.state, on ? UI_COL_BAD : UI_COL_TEXT_DIM, 0);
    lv_label_set_text(S.btn_lbl, on ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);
    lv_label_set_text_fmt(S.v_frames, "%lu", (unsigned long)st.frames);
    lv_label_set_text_fmt(S.v_pps, "%lu/s", (unsigned long)st.pps);
    lv_label_set_text_fmt(S.v_ch, "%d", st.channel);
    if (on) lv_obj_add_state(S.dd, LV_STATE_DISABLED);
    else lv_obj_remove_state(S.dd, LV_STATE_DISABLED);
}

static void do_start(void)
{
    int sel = lv_dropdown_get_selected(S.dd);
    if (sel < 0 || sel >= S.ap_count) {
        ui_toast("Pick a target AP first", UI_COL_BAD);
        return;
    }
    const wifi_ap_record_t *ap = &S.aps[sel];
    if (!wifi_attack_deauth_start(ap->bssid, ap->primary, nullptr))
        ui_toast("Could not start", UI_COL_BAD);
    update(nullptr);
}

static void on_toggle(lv_event_t *)
{
    if (wifi_attack_deauth_running()) {
        wifi_attack_deauth_stop();
        update(nullptr);
    } else {
        ui_confirm("Deauth flood",
                   "Disconnects clients from the selected AP. Only use on networks you are "
                   "authorized to test. This drops your own Wi-Fi link.",
                   "Start", UI_COL_BAD, do_start);
    }
}

lv_obj_t *ui_deauth_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Deauth", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = ui_caption(content, "TARGET ACCESS POINT");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    S.dd = lv_dropdown_create(content);
    lv_obj_set_width(S.dd, LV_PCT(100));
    lv_obj_align(S.dd, LV_ALIGN_TOP_LEFT, 0, 16);
    fill_dropdown();
    S.seen_gen = wifi_mgr_scan_gen();

    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *row = ui_kv_row(card, "State");
    S.state = row;
    S.v_frames = ui_kv_row(card, "Frames sent");
    S.v_pps = ui_kv_row(card, "Rate");
    S.v_ch = ui_kv_row(card, "Channel");

    S.warn = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                      "Deauth + disassoc flood on the AP's channel. Note: routers/clients with 802.11w "
                      "(PMF) - common on WPA3 and modern WPA2 - ignore these frames and cannot be knocked off.");
    lv_label_set_long_mode(S.warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(S.warn, LV_PCT(100));
    lv_obj_align(S.warn, LV_ALIGN_BOTTOM_LEFT, 0, -34);

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 110, 30);
    lv_obj_align(S.btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    lv_timer_t *t = ui_screen_timer(content, update, 500, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
