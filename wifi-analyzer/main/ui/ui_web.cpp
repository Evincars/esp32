#include <cstdio>
#include <cstring>

#include "app_settings.h"
#include "ui.h"
#include "web_server.h"
#include "wifi_mgr.h"

static struct {
    lv_obj_t *state, *url, *hint, *qr, *btn, *btn_lbl;
    lv_obj_t *v_requests, *v_client;
    char shown_url[40];
} S;

static void update(lv_timer_t *)
{
    bool on = web_server_running();
    wifi_conn_info_t info;
    bool up = wifi_mgr_conn_info(&info);

    char url[40] = "";
    if (up) snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&info.ip.ip));

    lv_label_set_text(S.state, on ? "Running" : "Stopped");
    lv_obj_set_style_text_color(S.state, on ? UI_COL_GOOD : UI_COL_TEXT_DIM, 0);
    lv_label_set_text(S.btn_lbl, on ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);

    if (on && up) {
        lv_label_set_text(S.url, url);
        lv_label_set_text(S.hint, "Open on a phone or PC in the same network, or scan the code.");
    } else if (on) {
        lv_label_set_text(S.url, "waiting for an IP address");
        lv_label_set_text(S.hint, "Connect to a Wi-Fi network; the dashboard is served on port 80.");
    } else {
        lv_label_set_text(S.url, up ? url : "-");
        lv_label_set_text(S.hint, "Serves system metrics, Wi-Fi, APs, LAN hosts, port scan results and captured packets.");
    }

    bool show_qr = on && up;
    if (show_qr && strcmp(url, S.shown_url) != 0) {
        lv_qrcode_update(S.qr, url, strlen(url));
        strlcpy(S.shown_url, url, sizeof(S.shown_url));
    }
    if (show_qr) lv_obj_remove_flag(S.qr, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.qr, LV_OBJ_FLAG_HIDDEN);

    char client[16];
    web_server_last_client(client, sizeof(client));
    lv_label_set_text_fmt(S.v_requests, "%lu", (unsigned long)web_server_requests());
    lv_label_set_text(S.v_client, client[0] ? client : "-");
}

static void on_toggle(lv_event_t *)
{
    if (web_server_running()) web_server_stop();
    else if (!web_server_start()) ui_toast("Could not start the server", UI_COL_BAD);
    update(nullptr);
}

static void on_autostart(lv_event_t *e)
{
    g_settings.web_autostart = lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
    app_settings_save();
}

lv_obj_t *ui_web_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Web server", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), 110);
    lv_obj_set_style_pad_all(card, 8, 0);

    S.qr = lv_qrcode_create(card);
    lv_qrcode_set_size(S.qr, 90);
    lv_qrcode_set_dark_color(S.qr, lv_color_black());
    lv_qrcode_set_light_color(S.qr, lv_color_white());
    lv_obj_set_style_border_color(S.qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(S.qr, 2, 0);
    lv_obj_align(S.qr, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *cap = ui_caption(card, "HTTP DASHBOARD");
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);
    S.state = ui_label(card, &lv_font_montserrat_16, UI_COL_TEXT, "");
    lv_obj_align(S.state, LV_ALIGN_TOP_LEFT, 0, 14);
    S.url = ui_label(card, &lv_font_montserrat_12, UI_COL_ACCENT, "");
    lv_label_set_long_mode(S.url, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.url, 190);
    lv_obj_align(S.url, LV_ALIGN_TOP_LEFT, 0, 38);
    S.hint = ui_label(card, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(S.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(S.hint, 190);
    lv_obj_align(S.hint, LV_ALIGN_TOP_LEFT, 0, 58);

    lv_obj_t *info = ui_card(content);
    lv_obj_set_size(info, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 0, 116);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    S.v_requests = ui_kv_row(info, "Requests served");
    S.v_client = ui_kv_row(info, "Last client");

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 100, 30);
    lv_obj_align(S.btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *sw = lv_switch_create(content);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    if (g_settings.web_autostart) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_autostart, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *swl = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "start at boot");
    lv_obj_align_to(swl, sw, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_timer_t *t = ui_screen_timer(content, update, 1000, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
