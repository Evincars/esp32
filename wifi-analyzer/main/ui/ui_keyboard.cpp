#include <cstdio>
#include <cstring>

#include "ui.h"
#include "wifi_mgr.h"

#define TA_H 36

static struct {
    lv_obj_t *ta;
    lv_obj_t *eye;
    ui_text_done_cb_t on_done;
    void *user;
    ui_screen_id_t back_to;
    char text[65];
} S;

static char s_flow_ssid[33];
static bool s_flow_hidden;

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        strlcpy(S.text, lv_textarea_get_text(S.ta), sizeof(S.text));
        if (S.on_done) S.on_done(S.text, S.user);
    } else if (code == LV_EVENT_CANCEL) {
        ui_go(S.back_to); /* the keyboard is modal: return to the screen that opened it */
    }
}

static void on_eye_clicked(lv_event_t *)
{
    bool hidden = lv_textarea_get_password_mode(S.ta);
    lv_textarea_set_password_mode(S.ta, !hidden);
    lv_label_set_text(lv_obj_get_child(S.eye, 0), hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void style_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, UI_COL_HEADER, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 3, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);

    lv_obj_set_style_bg_color(kb, UI_COL_CARD_HI, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 3, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    /* modifier keys (shift, mode, backspace, OK...) */
    lv_obj_set_style_bg_color(kb, UI_COL_BORDER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, UI_COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, UI_COL_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, UI_COL_ON_FILL, LV_PART_ITEMS | LV_STATE_PRESSED);
}

void ui_text_input(const char *title, const char *placeholder, const char *initial, bool password,
                   ui_text_done_cb_t on_done, void *user, ui_screen_id_t back_to)
{
    memset(&S, 0, sizeof(S));
    S.on_done = on_done;
    S.user = user;
    S.back_to = back_to;

    lv_obj_t *content = ui_screen_create(title, true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *scr = lv_obj_get_screen(content);

    S.ta = lv_textarea_create(content);
    lv_textarea_set_one_line(S.ta, true);
    lv_textarea_set_max_length(S.ta, 64);
    lv_textarea_set_placeholder_text(S.ta, placeholder);
    lv_textarea_set_password_mode(S.ta, password);
    lv_textarea_set_password_show_time(S.ta, 900); /* briefly show each typed character */
    lv_textarea_set_text(S.ta, initial ? initial : "");
    lv_obj_set_size(S.ta, password ? 262 : LV_PCT(100), TA_H);
    lv_obj_align(S.ta, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(S.ta, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(S.ta, UI_COL_ACCENT, 0);
    lv_obj_set_style_border_width(S.ta, 1, 0);
    lv_obj_set_style_radius(S.ta, UI_RADIUS, 0);
    lv_obj_set_style_text_color(S.ta, UI_COL_TEXT, 0);
    lv_obj_set_style_text_font(S.ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_ver(S.ta, 7, 0);
    lv_obj_set_style_border_color(S.ta, UI_COL_ACCENT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_state(S.ta, LV_STATE_FOCUSED); /* shows the blinking cursor */

    if (password) {
        S.eye = lv_button_create(content);
        lv_obj_set_size(S.eye, 40, TA_H);
        lv_obj_align(S.eye, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(S.eye, UI_COL_CARD_HI, 0);
        lv_obj_set_style_radius(S.eye, UI_RADIUS, 0);
        lv_obj_set_style_shadow_width(S.eye, 0, 0);
        lv_obj_t *l = ui_label(S.eye, &lv_font_montserrat_16, UI_COL_ACCENT, LV_SYMBOL_EYE_OPEN);
        lv_obj_center(l);
        lv_obj_add_event_cb(S.eye, on_eye_clicked, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, LV_PCT(100), 240 - UI_HEADER_H - TA_H - 12);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_popovers(kb, true);
    lv_keyboard_set_textarea(kb, S.ta);
    style_keyboard(kb);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, nullptr);
    ui_anim_enter(kb, 60);

    ui_load(scr, true);
}

/* ---------------- connect flow ---------------- */

static void do_connect(const char *ssid, const char *pass)
{
    wifi_mgr_connect(ssid, pass);
    ui_go(UI_SCR_NETWORK);
}

static void on_password(const char *text, void *)
{
    if (!s_flow_hidden && text[0] == 0) {
        ui_toast("Enter the network password", UI_COL_ACCENT);
        return;
    }
    size_t len = strlen(text);
    if (len > 0 && len < 8 && len != 5 && len != 13) { /* 5/13 = WEP keys */
        ui_toast("WPA passwords have at least 8 characters", UI_COL_ACCENT);
        return;
    }
    do_connect(s_flow_ssid, text);
}

static void ask_password(ui_screen_id_t back_to)
{
    char known[65] = "";
    wifi_mgr_known_password(s_flow_ssid, known, sizeof(known));
    char title[48];
    snprintf(title, sizeof(title), "%.28s", s_flow_ssid);
    ui_text_input(title, s_flow_hidden ? "Password (empty = open)" : "Password", known, true, on_password, nullptr,
                  back_to);
}

static void on_hidden_ssid(const char *text, void *)
{
    if (text[0] == 0) {
        ui_toast("Enter the network name", UI_COL_ACCENT);
        return;
    }
    strlcpy(s_flow_ssid, text, sizeof(s_flow_ssid));
    ask_password(UI_SCR_NETWORK);
}

void ui_connect_flow(const char *ssid, bool needs_password)
{
    s_flow_hidden = ssid[0] == 0;
    if (s_flow_hidden) {
        ui_text_input("Hidden network", "Network name (SSID)", "", false, on_hidden_ssid, nullptr,
                      UI_SCR_NETWORK);
        return;
    }
    strlcpy(s_flow_ssid, ssid, sizeof(s_flow_ssid));
    if (needs_password) ask_password(UI_SCR_SCANNER);
    else do_connect(s_flow_ssid, "");
}
