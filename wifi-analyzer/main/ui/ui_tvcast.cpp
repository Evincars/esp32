#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tvcast.h"
#include "ui.h"
#include "wifi_mgr.h"

/* survive screen rebuilds */
static tv_device_t s_devs[TV_MAX_DEVICES];
static int s_count;
static int s_sel = -1;
static volatile bool s_scanning;
static volatile bool s_busy;   /* a launch/stop request is in flight */
static volatile int s_last_http;
static char s_last_msg[40];

/* pending action for the worker task */
static tv_device_t s_act_dev;
static char s_act_app[24];
static char s_act_body[80];
static int s_act_kind; /* 0 launch, 1 stop, 2 youtube */

static struct {
    lv_obj_t *status, *list, *controls, *vid_btn, *yt_btn, *nf_btn, *stop_btn, *result;
} S;

/* ---------------- background workers ---------------- */

static void discover_task(void *)
{
    s_count = tvcast_discover(s_devs, TV_MAX_DEVICES, 3000);
    if (s_sel >= s_count) s_sel = -1;
    s_scanning = false;
    vTaskDelete(nullptr);
}

static void action_task(void *)
{
    int st = -1;
    if (s_act_kind == 2) st = tvcast_youtube(&s_act_dev, s_act_body);
    else if (s_act_kind == 1) st = tvcast_stop(&s_act_dev, s_act_app);
    else st = tvcast_launch(&s_act_dev, s_act_app, s_act_body[0] ? s_act_body : nullptr);
    s_last_http = st;
    s_busy = false;
    vTaskDelete(nullptr);
}

static void start_action(int kind, const char *app, const char *body, const char *msg)
{
    if (s_busy || s_sel < 0) return;
    s_act_dev = s_devs[s_sel];
    s_act_kind = kind;
    snprintf(s_act_app, sizeof(s_act_app), "%s", app ? app : "");
    snprintf(s_act_body, sizeof(s_act_body), "%s", body ? body : "");
    snprintf(s_last_msg, sizeof(s_last_msg), "%s", msg);
    s_last_http = 0;
    s_busy = true;
    xTaskCreatePinnedToCore(action_task, "tv_act", 4096, nullptr, 4, nullptr, 0);
}

/* ---------------- events ---------------- */

static void on_discover(lv_event_t *)
{
    if (s_scanning) return;
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
        ui_toast("Connect to Wi-Fi first", UI_COL_ACCENT);
        return;
    }
    s_scanning = true;
    xTaskCreatePinnedToCore(discover_task, "tv_disc", 4096, nullptr, 4, nullptr, 0);
}

static void on_row(lv_event_t *e)
{
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    ui_go(UI_SCR_TVCAST); /* rebuild so the controls bind to the new selection */
}

static void on_video_done(const char *text, void *)
{
    if (text && text[0]) start_action(2, "YouTube", text, "Playing video");
    ui_go(UI_SCR_TVCAST);
}

static void on_video(lv_event_t *)
{
    ui_text_input("YouTube video", "video id (e.g. dQw4w9WgXcQ)", "", false, on_video_done, nullptr, UI_SCR_TVCAST);
}

static void on_youtube(lv_event_t *) { start_action(2, "YouTube", "", "Opening YouTube"); }
static void on_netflix(lv_event_t *) { start_action(0, "Netflix", nullptr, "Opening Netflix"); }
static void on_stop(lv_event_t *) { start_action(1, "YouTube", nullptr, "Stopping YouTube"); }

/* ---------------- build ---------------- */

static void rebuild_list(void)
{
    lv_obj_clean(S.list);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = lv_button_create(S.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 34);
        lv_obj_set_style_bg_color(row, i == s_sel ? UI_COL_CARD_HI : UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_set_style_border_color(row, i == s_sel ? UI_COL_ACCENT : UI_COL_BORDER, 0);
        lv_obj_set_style_border_width(row, i == s_sel ? 2 : 1, 0);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = ui_label(row, &lv_font_montserrat_14, UI_COL_TEXT, s_devs[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 200);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);

        esp_ip4_addr_t ip = {s_devs[i].ip};
        lv_obj_t *ipl = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(ipl, IPSTR, IP2STR(&ip));
        lv_obj_align(ipl, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

static void poll(lv_timer_t *)
{
    if (s_scanning) {
        lv_label_set_text(S.status, "Searching (SSDP)...");
    } else {
        lv_label_set_text_fmt(S.status, s_count ? "%d device(s) - tap to select" : "No devices. Tap Discover.",
                              s_count);
    }
    static int shown = -2;
    if (!s_scanning && shown != s_count) {
        shown = s_count;
        rebuild_list();
    }

    bool sel = s_sel >= 0 && s_sel < s_count;
    lv_obj_set_style_opa(S.controls, sel ? LV_OPA_COVER : LV_OPA_50, 0);
    if (S.yt_btn) {
        if (sel && !s_busy) {
            lv_obj_remove_state(S.yt_btn, LV_STATE_DISABLED);
            lv_obj_remove_state(S.vid_btn, LV_STATE_DISABLED);
            lv_obj_remove_state(S.nf_btn, LV_STATE_DISABLED);
            lv_obj_remove_state(S.stop_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(S.yt_btn, LV_STATE_DISABLED);
            lv_obj_add_state(S.vid_btn, LV_STATE_DISABLED);
            lv_obj_add_state(S.nf_btn, LV_STATE_DISABLED);
            lv_obj_add_state(S.stop_btn, LV_STATE_DISABLED);
        }
    }
    if (s_busy) lv_label_set_text_fmt(S.result, "%s...", s_last_msg);
    else if (s_last_http) {
        bool ok = s_last_http >= 200 && s_last_http < 300;
        lv_label_set_text_fmt(S.result, "%s: HTTP %d %s", s_last_msg, s_last_http, ok ? "OK" : "");
        lv_obj_set_style_text_color(S.result, ok ? UI_COL_GOOD : UI_COL_BAD, 0);
    }
}

lv_obj_t *ui_tvcast_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("TV cast", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disc = ui_button(content, "Discover", UI_COL_ACCENT);
    lv_obj_set_size(disc, 100, 28);
    lv_obj_align(disc, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(disc, on_discover, LV_EVENT_CLICKED, nullptr);

    S.status = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "Tap Discover to find DIAL devices");
    lv_label_set_long_mode(S.status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.status, 200);
    lv_obj_align(S.status, LV_ALIGN_TOP_LEFT, 0, 6);

    S.list = lv_obj_create(content);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_size(S.list, LV_PCT(100), 96);
    lv_obj_align(S.list, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 4, 0);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_ACTIVE);

    /* controls */
    S.controls = lv_obj_create(content);
    lv_obj_remove_style_all(S.controls);
    lv_obj_set_size(S.controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(S.controls, LV_ALIGN_TOP_LEFT, 0, 136);
    lv_obj_set_flex_flow(S.controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(S.controls, 6, 0);
    lv_obj_set_style_pad_column(S.controls, 6, 0);

    S.yt_btn = ui_button(S.controls, "YouTube", UI_COL_BAD);
    lv_obj_set_size(S.yt_btn, 100, 32);
    lv_obj_add_event_cb(S.yt_btn, on_youtube, LV_EVENT_CLICKED, nullptr);
    S.vid_btn = ui_button(S.controls, "Play video", UI_COL_ACCENT);
    lv_obj_set_size(S.vid_btn, 108, 32);
    lv_obj_add_event_cb(S.vid_btn, on_video, LV_EVENT_CLICKED, nullptr);
    S.nf_btn = ui_button_outline(S.controls, "Netflix");
    lv_obj_set_size(S.nf_btn, 92, 32);
    lv_obj_add_event_cb(S.nf_btn, on_netflix, LV_EVENT_CLICKED, nullptr);
    S.stop_btn = ui_button_outline(S.controls, "Stop YT");
    lv_obj_set_size(S.stop_btn, 92, 32);
    lv_obj_add_event_cb(S.stop_btn, on_stop, LV_EVENT_CLICKED, nullptr);

    S.result = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(S.result, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.result, LV_PCT(100));
    lv_obj_align(S.result, LV_ALIGN_TOP_LEFT, 0, 182);

    lv_obj_t *hint = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                              "DIAL launches apps on the TV. Free-text search isn't part of DIAL - use a video id.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_timer_t *t = ui_screen_timer(content, poll, 300, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}
