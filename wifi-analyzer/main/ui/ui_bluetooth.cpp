#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bt_mgr.h"
#include "esp_timer.h"
#include "ui.h"

#define SEND_MAX 120

static struct {
    lv_obj_t *scan_tab, *dev_tab, *bcast_tab;
    /* scan */
    lv_obj_t *scan_btn, *scan_lbl, *list;
    uint32_t scan_seen;
    /* device / peripheral */
    lv_obj_t *dev_state, *dev_adv_btn, *dev_send_btn, *console;
    uint32_t rx_seen;
    /* broadcast */
    lv_obj_t *bc_state, *bc_btn, *bc_count;
} S;

static bt_dev_t s_devs[BT_MAX_DEVS];

/* ---------------- scan tab ---------------- */

static void fmt_mac(char *b, size_t n, const uint8_t m[6])
{
    snprintf(b, n, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bt_dev_t s_sel;

static void detail_close(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
}

static void open_detail(const bt_dev_t *d)
{
    s_sel = *d;
    char body[512];
    char mac[20];
    fmt_mac(mac, sizeof(mac), d->addr);
    int k = snprintf(body, sizeof(body),
                     "MAC: %s (%s%s)\nRSSI: %d dBm\nType: %s\n", mac, d->addr_type ? "random" : "public",
                     d->random_private ? ", private" : "", d->rssi, d->connectable ? "connectable" : "broadcast-only");
    const char *cls = bt_classify(d);
    if (cls[0]) k += snprintf(body + k, sizeof(body) - k, "Class: %s\n", cls);
    const char *co = bt_company_name(d->company);
    if (co[0]) k += snprintf(body + k, sizeof(body) - k, "Vendor: %s (0x%04X)\n", co, d->company);
    else if (d->company != 0xFFFF) k += snprintf(body + k, sizeof(body) - k, "Vendor id: 0x%04X\n", d->company);
    if (d->appearance) k += snprintf(body + k, sizeof(body) - k, "Appearance: 0x%04X\n", d->appearance);
    if (d->n_svc16) {
        k += snprintf(body + k, sizeof(body) - k, "Services:");
        for (int i = 0; i < d->n_svc16; i++) k += snprintf(body + k, sizeof(body) - k, " 0x%04X", d->svc16[i]);
        k += snprintf(body + k, sizeof(body) - k, "\n");
    }
    k += snprintf(body + k, sizeof(body) - k, "\nAdv data (%d B):\n", d->adv_len);
    for (int i = 0; i < d->adv_len && k < (int)sizeof(body) - 4; i++)
        k += snprintf(body + k, sizeof(body) - k, "%02X ", d->adv[i]);

    lv_obj_t *mb = lv_msgbox_create(nullptr);
    lv_obj_set_size(mb, 300, 220);
    lv_obj_set_style_bg_color(mb, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(mb, UI_COL_ACCENT, 0);
    lv_obj_set_style_border_width(mb, 1, 0);
    lv_msgbox_add_title(mb, d->name[0] ? d->name : "(no name)");
    lv_obj_t *txt = lv_msgbox_add_text(mb, body);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_12, 0);
    lv_obj_t *close = lv_msgbox_add_footer_button(mb, "Close");
    lv_obj_set_style_bg_color(close, UI_COL_CARD_HI, 0);
    lv_obj_add_event_cb(close, detail_close, LV_EVENT_CLICKED, mb);
}

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < bt_scan_count()) open_detail(&s_devs[i]);
}

static void rebuild_list(void)
{
    int n = bt_scan_get(s_devs, BT_MAX_DEVS);
    std::sort(s_devs, s_devs + n, [](const bt_dev_t &a, const bt_dev_t &b) { return a.rssi > b.rssi; });

    /* keep the user's scroll position: the list is rebuilt on every new/updated
       device while scanning, which would otherwise snap back to the top each time */
    int32_t saved_y = lv_obj_get_scroll_y(S.list);
    lv_obj_clean(S.list);
    for (int i = 0; i < n; i++) {
        const bt_dev_t &d = s_devs[i];
        lv_obj_t *row = lv_button_create(S.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 38);
        lv_obj_set_style_bg_color(row, UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, UI_COL_CARD_HI, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = ui_label(row, &lv_font_montserrat_14, d.name[0] ? UI_COL_TEXT : UI_COL_TEXT_DIM,
                                  d.name[0] ? d.name : "(no name)");
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 170);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 6, 3);

        char mac[20];
        fmt_mac(mac, sizeof(mac), d.addr);
        const char *cls = bt_classify(&d);
        lv_obj_t *sub = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(sub, "%s%s%s%s", mac, d.random_private ? " (random)" : "", cls[0] ? "  " : "", cls);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, 240);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 6, -3);

        lv_obj_t *rssi = ui_label(row, &lv_font_montserrat_14, ui_rssi_color(d.rssi), "");
        lv_label_set_text_fmt(rssi, "%d", d.rssi);
        lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    /* restore scroll (clamped by LVGL to the new content height) */
    lv_obj_update_layout(S.list);
    lv_obj_scroll_to_y(S.list, saved_y, LV_ANIM_OFF);
}

static void on_scan_toggle(lv_event_t *)
{
    if (bt_scanning()) bt_scan_stop();
    else if (!bt_scan_start()) ui_toast("BLE not ready / busy broadcasting", UI_COL_ACCENT);
}

static void build_scan_tab(lv_obj_t *tab)
{
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    S.scan_lbl = ui_label(tab, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "0 devices");
    lv_obj_align(S.scan_lbl, LV_ALIGN_TOP_LEFT, 0, 4);

    S.scan_btn = ui_button(tab, "Scan", UI_COL_ACCENT);
    lv_obj_set_size(S.scan_btn, 80, 28);
    lv_obj_align(S.scan_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(S.scan_btn, on_scan_toggle, LV_EVENT_CLICKED, nullptr);

    S.list = lv_obj_create(tab);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_size(S.list, LV_PCT(100), UI_CONTENT_H - 28 - 34);
    lv_obj_align(S.list, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.list, 4, 0);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_ACTIVE);
}

/* ---------------- device (peripheral) tab ---------------- */

static void on_name_done(const char *text, void *)
{
    if (text && text[0]) bt_peripheral_start(text); /* (re)advertise under the new name */
    ui_go(UI_SCR_BLUETOOTH);
}

static void on_edit_name(lv_event_t *)
{
    ui_text_input("Device name", "esp32-s3-ble", bt_peripheral_name(), false, on_name_done, nullptr, UI_SCR_BLUETOOTH);
}

static void on_adv_toggle(lv_event_t *)
{
    if (bt_peripheral_running()) bt_peripheral_stop();
    else if (!bt_peripheral_start(nullptr)) ui_toast("BLE not ready / busy broadcasting", UI_COL_ACCENT);
}

static void on_send_done(const char *text, void *)
{
    if (text && text[0]) bt_peripheral_send(text);
    ui_go(UI_SCR_BLUETOOTH);
}

static void on_send(lv_event_t *)
{
    if (!bt_connected()) {
        ui_toast("No central connected", UI_COL_ACCENT);
        return;
    }
    ui_text_input("Send over BLE", "message to the phone", "", false, on_send_done, nullptr, UI_SCR_BLUETOOTH);
}

static void build_dev_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 6, 0);

    lv_obj_t *card = ui_card(tab);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    S.dev_state = ui_kv_row(card, "State");
    lv_label_set_text(ui_kv_row(card, "Service"), "Nordic UART");

    lv_obj_t *row = lv_obj_create(tab);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    S.dev_adv_btn = ui_button(row, "Advertise", UI_COL_ACCENT);
    lv_obj_set_flex_grow(S.dev_adv_btn, 1);
    lv_obj_set_height(S.dev_adv_btn, 30);
    lv_obj_add_event_cb(S.dev_adv_btn, on_adv_toggle, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *en = ui_button_outline(row, "Name");
    lv_obj_set_size(en, 70, 30);
    lv_obj_add_event_cb(en, on_edit_name, LV_EVENT_CLICKED, nullptr);
    S.dev_send_btn = ui_button_outline(row, "Send");
    lv_obj_set_size(S.dev_send_btn, 70, 30);
    lv_obj_add_event_cb(S.dev_send_btn, on_send, LV_EVENT_CLICKED, nullptr);

    ui_caption(tab, "RECEIVED FROM PHONE");
    lv_obj_t *cc = ui_card(tab);
    lv_obj_set_size(cc, LV_PCT(100), UI_CONTENT_H - 28 - 44 - 36 - 20);
    lv_obj_set_scrollbar_mode(cc, LV_SCROLLBAR_MODE_AUTO);
    S.console = ui_label(cc, &lv_font_unscii_8, UI_COL_GOOD, "");
    lv_label_set_long_mode(S.console, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(S.console, LV_PCT(100));

    lv_obj_t *hint = ui_label(tab, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                              "Connect from a BLE UART app (e.g. nRF Connect) to exchange text.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
}

/* ---------------- broadcast tab ---------------- */

static void do_bcast(void)
{
    if (!bt_broadcast_start()) ui_toast("Could not start", UI_COL_BAD);
}

static void on_bcast_toggle(lv_event_t *)
{
    if (bt_broadcast_running()) {
        bt_broadcast_stop();
    } else {
        ui_confirm("BLE broadcast",
                   "Transmits many non-connectable BLE advertisements (airtime/RF test). Use only on "
                   "hardware and spectrum you are authorized to test. Stops scanning and advertising.",
                   "Start", UI_COL_BAD, do_bcast);
    }
}

static void build_bcast_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 8, 0);

    lv_obj_t *card = ui_card(tab);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    S.bc_state = ui_kv_row(card, "State");
    S.bc_count = ui_kv_row(card, "Adverts sent");

    lv_obj_t *warn = ui_label(tab, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                              "Non-connectable advertisements with rotating random addresses and a benign payload. "
                              "This is an RF airtime test for authorized use - not pairing/pop-up spam.");
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));

    S.bc_btn = ui_button(tab, "Start", UI_COL_BAD);
    lv_obj_set_size(S.bc_btn, LV_PCT(100), 34);
    lv_obj_add_event_cb(S.bc_btn, on_bcast_toggle, LV_EVENT_CLICKED, nullptr);
}

/* ---------------- poll + lifecycle ---------------- */

static void poll(lv_timer_t *)
{
    /* scan */
    bool sc = bt_scanning();
    lv_label_set_text(lv_obj_get_child(S.scan_btn, 0), sc ? "Stop" : "Scan");
    lv_obj_set_style_bg_color(S.scan_btn, sc ? UI_COL_BAD : UI_COL_ACCENT, 0);
    lv_label_set_text_fmt(S.scan_lbl, "%d devices%s", bt_scan_count(), sc ? "  scanning" : "");
    if (bt_scan_gen() != S.scan_seen) {
        S.scan_seen = bt_scan_gen();
        rebuild_list();
    }

    /* device */
    bool adv = bt_peripheral_running(), conn = bt_connected();
    lv_label_set_text(S.dev_state, conn ? "Connected" : adv ? "Advertising" : "Off");
    lv_obj_set_style_text_color(S.dev_state, conn ? UI_COL_GOOD : adv ? UI_COL_ACCENT : UI_COL_TEXT_DIM, 0);
    lv_label_set_text(lv_obj_get_child(S.dev_adv_btn, 0), adv ? "Stop" : "Advertise");
    lv_obj_set_style_bg_color(S.dev_adv_btn, adv ? UI_COL_BAD : UI_COL_ACCENT, 0);
    if (conn) lv_obj_remove_state(S.dev_send_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(S.dev_send_btn, LV_STATE_DISABLED);
    if (bt_rx_gen() != S.rx_seen) {
        S.rx_seen = bt_rx_gen();
        static char buf[BT_RX_BUF];
        bt_rx_text(buf, sizeof(buf));
        lv_label_set_text(S.console, buf[0] ? buf : "(nothing yet)");
    }

    /* broadcast */
    bool bc = bt_broadcast_running();
    lv_label_set_text(S.bc_state, bc ? "Broadcasting" : "Idle");
    lv_obj_set_style_text_color(S.bc_state, bc ? UI_COL_BAD : UI_COL_TEXT_DIM, 0);
    lv_label_set_text_fmt(S.bc_count, "%lu", (unsigned long)bt_broadcast_count());
    lv_label_set_text(lv_obj_get_child(S.bc_btn, 0), bc ? "Stop" : "Start");
}

static void on_delete(lv_event_t *)
{
    if (bt_scanning()) bt_scan_stop(); /* free the radio when leaving; peripheral/broadcast persist */
}

static uint32_t s_bt_tab; /* remember Scan/Device/Broadcast across visits */

static void on_tab_changed(lv_event_t *e)
{
    s_bt_tab = lv_tabview_get_tab_active((lv_obj_t *)lv_event_get_target(e));
}

static void style_tabview(lv_obj_t *tv)
{
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, UI_COL_HEADER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    uint32_t n = lv_obj_get_child_count(bar);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(bar, i);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(btn, UI_COL_TEXT_DIM, 0);
        lv_obj_set_style_text_color(btn, UI_COL_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, UI_COL_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_opa(lv_tabview_get_content(tv), LV_OPA_TRANSP, 0);
}

lv_obj_t *ui_bluetooth_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Bluetooth LE", true);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(content, on_delete, LV_EVENT_DELETE, nullptr);

    if (!bt_ready()) {
        lv_obj_t *l = ui_label(content, &lv_font_montserrat_14, UI_COL_TEXT_DIM, "BLE controller starting...");
        lv_obj_center(l);
        return lv_obj_get_screen(content);
    }

    lv_obj_t *tv = lv_tabview_create(content);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_size(tv, 28);
    S.scan_tab = lv_tabview_add_tab(tv, "Scan");
    S.dev_tab = lv_tabview_add_tab(tv, "Device");
    S.bcast_tab = lv_tabview_add_tab(tv, "Broadcast");
    style_tabview(tv);
    lv_obj_set_style_pad_all(S.scan_tab, 6, 0);
    lv_obj_set_style_pad_all(S.dev_tab, 6, 0);
    lv_obj_set_style_pad_all(S.bcast_tab, 6, 0);

    build_scan_tab(S.scan_tab);
    build_dev_tab(S.dev_tab);
    build_bcast_tab(S.bcast_tab);
    lv_tabview_set_active(tv, s_bt_tab, LV_ANIM_OFF);
    lv_obj_add_event_cb(tv, on_tab_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    if (!bt_scanning() && !bt_broadcast_running()) bt_scan_start();
    lv_timer_t *t = ui_screen_timer(content, poll, 400, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}
