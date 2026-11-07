#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ssh_server.h"
#include "ui.h"
#include "wifi_mgr.h"

static struct {
    lv_obj_t *state, *port, *user, *hostkey, *clients, *connect, *toggle;
} S;

/* ---------------- edit callbacks ---------------- */

static void on_user_done(const char *text, void *)
{
    if (text && text[0]) ssh_server_configure(ssh_server_enabled(), ssh_server_port(), text, nullptr);
    ui_go(UI_SCR_SSH);
}
static void on_edit_user(lv_event_t *)
{
    char u[SSH_USER_MAX];
    ssh_server_user(u, sizeof(u));
    ui_text_input("SSH user", "login name", u, false, on_user_done, nullptr, UI_SCR_SSH);
}

static void on_pass_done(const char *text, void *)
{
    if (text && text[0]) ssh_server_configure(ssh_server_enabled(), ssh_server_port(), nullptr, text);
    ui_go(UI_SCR_SSH);
}
static void on_edit_pass(lv_event_t *)
{
    ui_text_input("SSH password", "min 1 char", "", true, on_pass_done, nullptr, UI_SCR_SSH);
}

static void on_port_done(const char *text, void *)
{
    int p = atoi(text);
    if (p > 0 && p < 65536) {
        char u[SSH_USER_MAX];
        ssh_server_user(u, sizeof(u));
        ssh_server_configure(ssh_server_enabled(), (uint16_t)p, u, nullptr);
    } else {
        ui_toast("Port must be 1-65535", UI_COL_BAD);
    }
    ui_go(UI_SCR_SSH);
}
static void on_edit_port(lv_event_t *)
{
    char p[8];
    snprintf(p, sizeof(p), "%u", ssh_server_port());
    ui_text_input("SSH port", "e.g. 22", p, false, on_port_done, nullptr, UI_SCR_SSH);
}

static void on_toggle(lv_event_t *)
{
    char u[SSH_USER_MAX];
    ssh_server_user(u, sizeof(u));
    ssh_server_configure(!ssh_server_enabled(), ssh_server_port(), u, nullptr);
}

static void do_regen(void)
{
    if (ssh_server_regen_hostkey()) ui_toast("New host key generated", UI_COL_GOOD);
    else ui_toast("Key generation failed", UI_COL_BAD);
}
static void on_regen(lv_event_t *)
{
    ui_confirm("Regenerate host key",
               "Clients that connected before will warn about a changed host key until you clear the old one.",
               "Regenerate", UI_COL_ACCENT, do_regen);
}

/* ---------------- poll ---------------- */

static void poll(lv_timer_t *)
{
    bool run = ssh_server_running();
    lv_label_set_text(S.state, run ? "Running" : ssh_server_enabled() ? "Enabled (starting...)" : "Stopped");
    lv_obj_set_style_text_color(S.state, run ? UI_COL_GOOD : UI_COL_TEXT_DIM, 0);

    lv_label_set_text_fmt(S.port, "%u", ssh_server_port());
    char u[SSH_USER_MAX];
    ssh_server_user(u, sizeof(u));
    lv_label_set_text(S.user, u);
    lv_label_set_text(S.hostkey, ssh_server_has_hostkey() ? "ECC (provisioned)" : "none yet");
    lv_label_set_text_fmt(S.clients, "%d", ssh_server_clients());

    lv_label_set_text(lv_obj_get_child(S.toggle, 0), ssh_server_enabled() ? "Disable" : "Enable");
    lv_obj_set_style_bg_color(S.toggle, ssh_server_enabled() ? UI_COL_BAD : UI_COL_GOOD, 0);

    wifi_conn_info_t info;
    if (wifi_mgr_conn_info(&info)) {
        char ip[16];
        esp_ip4_addr_t a = {info.ip.ip.addr};
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        lv_label_set_text_fmt(S.connect, "ssh %s@%s -p %u", u, ip, ssh_server_port());
    } else {
        lv_label_set_text(S.connect, "Connect to Wi-Fi to get an address");
    }
}

lv_obj_t *ui_ssh_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("SSH server", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    S.state = ui_kv_row(card, "State");
    S.port = ui_kv_row(card, "Port");
    S.user = ui_kv_row(card, "User");
    S.hostkey = ui_kv_row(card, "Host key");
    S.clients = ui_kv_row(card, "Clients");

    /* connect hint */
    lv_obj_t *cc = ui_card(content);
    lv_obj_set_size(cc, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cc, LV_FLEX_FLOW_COLUMN);
    ui_caption(cc, "CONNECT FROM A PC");
    S.connect = ui_label(cc, &lv_font_unscii_8, UI_COL_GOOD, "");
    lv_label_set_long_mode(S.connect, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(S.connect, LV_PCT(100));

    /* action rows */
    lv_obj_t *row1 = lv_obj_create(content);
    lv_obj_remove_style_all(row1);
    lv_obj_set_size(row1, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row1, 6, 0);
    S.toggle = ui_button(row1, "Enable", UI_COL_GOOD);
    lv_obj_set_flex_grow(S.toggle, 1);
    lv_obj_set_height(S.toggle, 32);
    lv_obj_add_event_cb(S.toggle, on_toggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *kb = ui_button_outline(row1, "Host key");
    lv_obj_set_size(kb, 92, 32);
    lv_obj_add_event_cb(kb, on_regen, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *row2 = lv_obj_create(content);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row2, 6, 0);
    lv_obj_t *bu = ui_button_outline(row2, "User");
    lv_obj_set_flex_grow(bu, 1);
    lv_obj_set_height(bu, 30);
    lv_obj_add_event_cb(bu, on_edit_user, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bp = ui_button_outline(row2, "Password");
    lv_obj_set_flex_grow(bp, 1);
    lv_obj_set_height(bp, 30);
    lv_obj_add_event_cb(bp, on_edit_pass, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bport = ui_button_outline(row2, "Port");
    lv_obj_set_flex_grow(bport, 1);
    lv_obj_set_height(bport, 30);
    lv_obj_add_event_cb(bport, on_edit_port, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *warn = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                              "Password SSH on your LAN. Encrypted (wolfSSH, software crypto) - the first "
                              "key exchange may take a few seconds on the ESP32.");
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));

    lv_timer_t *t = ui_screen_timer(content, poll, 500, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}
