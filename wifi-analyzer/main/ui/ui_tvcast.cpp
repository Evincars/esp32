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
static volatile bool s_busy; /* a launch/stop request is in flight */
static volatile int s_last_http;
static char s_last_msg[48];
static char s_last_app[24]; /* last app we acted on, for Stop/Status */

/* pending action for the worker task */
static tv_device_t s_act_dev;
static char s_act_app[24];
static char s_act_body[80];
static int s_act_kind; /* 0 launch, 1 stop, 2 youtube, 3 status */

static struct {
    lv_obj_t *status, *list, *result;
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
    char state[24] = "";
    int st = -1;
    if (s_act_kind == 2) st = tvcast_youtube(&s_act_dev, s_act_body);
    else if (s_act_kind == 1) st = tvcast_stop(&s_act_dev, s_act_app);
    else if (s_act_kind == 3) st = tvcast_app_state(&s_act_dev, s_act_app, state, sizeof(state));
    else st = tvcast_launch(&s_act_dev, s_act_app, s_act_body[0] ? s_act_body : nullptr);

    if (s_act_kind == 3)
        snprintf(s_last_msg, sizeof(s_last_msg), "%s is %s", s_act_app, state[0] ? state : "unknown");
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
    if (app && app[0]) snprintf(s_last_app, sizeof(s_last_app), "%s", app);
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
    ui_go(UI_SCR_TVCAST); /* rebuild so the selected device pins to the top */
}

static void on_app(lv_event_t *e)
{
    const char *app = (const char *)lv_event_get_user_data(e);
    char msg[48];
    snprintf(msg, sizeof(msg), "Opening %s", app);
    start_action(0, app, nullptr, msg);
}

static void on_youtube(lv_event_t *) { start_action(2, "YouTube", "", "Opening YouTube"); }
static void on_stop(lv_event_t *)
{
    start_action(1, s_last_app[0] ? s_last_app : "YouTube", nullptr, "Stopping app");
}
static void on_status(lv_event_t *)
{
    start_action(3, s_last_app[0] ? s_last_app : "YouTube", nullptr, "Checking");
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

static void on_custom_done(const char *text, void *)
{
    if (text && text[0]) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Opening %s", text);
        start_action(0, text, nullptr, msg);
    }
    ui_go(UI_SCR_TVCAST);
}
static void on_custom(lv_event_t *)
{
    ui_text_input("Launch app", "DIAL app name (e.g. Hulu)", "", false, on_custom_done, nullptr, UI_SCR_TVCAST);
}

/* ---------------- build helpers ---------------- */

static lv_obj_t *app_btn(lv_obj_t *parent, const char *label, lv_color_t col, const char *app_name)
{
    lv_obj_t *b = ui_button(parent, label, col);
    lv_obj_set_size(b, 98, 34);
    lv_obj_add_event_cb(b, on_app, LV_EVENT_CLICKED, (void *)app_name);
    return b;
}

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
        lv_obj_set_width(name, 196);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);

        esp_ip4_addr_t ip = {s_devs[i].ip};
        lv_obj_t *ipl = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(ipl, IPSTR, IP2STR(&ip));
        lv_obj_align(ipl, LV_ALIGN_RIGHT_MID, -8, 0);
    }
}

static void poll(lv_timer_t *)
{
    if (s_scanning) lv_label_set_text(S.status, "Searching (SSDP)...");
    else lv_label_set_text_fmt(S.status, s_count ? "%d device(s) - tap one" : "No devices. Tap Discover.", s_count);

    static int shown = -2;
    if (!s_scanning && shown != s_count) {
        shown = s_count;
        rebuild_list();
    }

    if (s_busy) lv_label_set_text_fmt(S.result, "%s...", s_last_msg);
    else if (s_last_http) {
        bool ok = s_last_http >= 200 && s_last_http < 300;
        if (s_act_kind == 3) lv_label_set_text_fmt(S.result, "%s (HTTP %d)", s_last_msg, s_last_http);
        else lv_label_set_text_fmt(S.result, "%s: HTTP %d %s", s_last_msg, s_last_http, ok ? "OK" : "failed");
        lv_obj_set_style_text_color(S.result, ok ? UI_COL_GOOD : UI_COL_BAD, 0);
    }
}

/* ---------------- screen ---------------- */

lv_obj_t *ui_tvcast_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("TV cast", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    /* header: status + Discover */
    lv_obj_t *hdr = lv_obj_create(content);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    S.status = ui_label(hdr, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "Tap Discover to find devices");
    lv_label_set_long_mode(S.status, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(S.status, 1);
    lv_obj_set_width(S.status, 1);
    lv_obj_t *disc = ui_button(hdr, "Discover", UI_COL_ACCENT);
    lv_obj_set_size(disc, 96, 28);
    lv_obj_add_event_cb(disc, on_discover, LV_EVENT_CLICKED, nullptr);

    bool sel = s_sel >= 0 && s_sel < s_count;

    /* selected device pinned on top */
    if (sel) {
        lv_obj_t *card = ui_card(content);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_border_color(card, UI_COL_ACCENT, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_t *nm = ui_label(card, &lv_font_montserrat_16, UI_COL_ACCENT, "");
        lv_label_set_text_fmt(nm, LV_SYMBOL_PLAY "  %s", s_devs[s_sel].name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, LV_PCT(100));
        esp_ip4_addr_t ip = {s_devs[s_sel].ip};
        lv_obj_t *sub = ui_label(card, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(sub, IPSTR "  port %u", IP2STR(&ip), s_devs[s_sel].port);
    }

    /* device list */
    ui_caption(content, "DEVICES");
    S.list = lv_obj_create(content);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_width(S.list, LV_PCT(100));
    lv_obj_set_height(S.list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 4, 0);

    /* controls - only meaningful once a device is picked */
    if (sel) {
        ui_caption(content, "CONTROL");
        lv_obj_t *grid = lv_obj_create(content);
        lv_obj_remove_style_all(grid);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(grid, 6, 0);
        lv_obj_set_style_pad_column(grid, 6, 0);

        app_btn(grid, "YouTube", UI_COL_BAD, "YouTube");
        lv_obj_t *pv = ui_button(grid, "Play video", UI_COL_ACCENT);
        lv_obj_set_size(pv, 98, 34);
        lv_obj_add_event_cb(pv, on_video, LV_EVENT_CLICKED, nullptr);
        app_btn(grid, "Netflix", UI_COL_BAD, "Netflix");
        app_btn(grid, "Prime", UI_COL_ACCENT, "PrimeVideo");
        app_btn(grid, "Disney+", UI_COL_ACCENT, "Disney");
        app_btn(grid, "Spotify", UI_COL_GOOD, "Spotify");

        lv_obj_t *cu = ui_button_outline(grid, "App...");
        lv_obj_set_size(cu, 98, 34);
        lv_obj_add_event_cb(cu, on_custom, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *stt = ui_button_outline(grid, "Status");
        lv_obj_set_size(stt, 98, 34);
        lv_obj_add_event_cb(stt, on_status, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *stp = ui_button_outline(grid, "Stop");
        lv_obj_set_size(stp, 98, 34);
        lv_obj_add_event_cb(stp, on_stop, LV_EVENT_CLICKED, nullptr);

        S.result = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
        lv_label_set_long_mode(S.result, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(S.result, LV_PCT(100));
    } else {
        /* keep a result label so poll() can always write to it */
        S.result = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                            "DIAL launches apps on the TV. Pick a device, then open an app. "
                            "Free-text search isn't part of DIAL - play YouTube by video id.");
        lv_label_set_long_mode(S.result, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(S.result, LV_PCT(100));
    }

    lv_timer_t *t = ui_screen_timer(content, poll, 300, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}
