#include <cstdio>
#include <cstring>

#include "esp_mac.h"
#include "esp_wifi.h"
#include "netid.h"
#include "oui.h"
#include "repeater.h"
#include "rogue_ap.h"
#include "ui.h"

static struct {
    lv_obj_t *v_sta, *v_ap, *v_bt, *v_label;
} S;

static void show_mac(lv_obj_t *lbl, wifi_interface_t ifx, esp_mac_type_t type, bool is_bt)
{
    uint8_t mac[6] = {};
    if (is_bt) esp_read_mac(mac, type);
    else esp_wifi_get_mac(ifx, mac);
    char m[20];
    ui_fmt_mac(m, sizeof(m), mac);
    const char *v = oui_vendor(mac);
    lv_label_set_text_fmt(lbl, "%s  %s", m, v ? v : "");
}

static void refresh(void)
{
    show_mac(S.v_sta, WIFI_IF_STA, ESP_MAC_WIFI_STA, false);
    show_mac(S.v_ap, WIFI_IF_AP, ESP_MAC_WIFI_SOFTAP, false);
    show_mac(S.v_bt, WIFI_IF_STA, ESP_MAC_BT, true);
    lv_label_set_text(S.v_label, netid_label());
    lv_obj_set_style_text_color(S.v_label, netid_spoofed() ? UI_COL_ACCENT : UI_COL_TEXT_DIM, 0);
}

static bool busy_guard(void)
{
    if (repeater_running() || rogue_ap_running()) {
        ui_toast("Stop the Repeater / Rogue AP first", UI_COL_BAD);
        return true;
    }
    return false;
}

static void on_randomize(lv_event_t *)
{
    if (busy_guard()) return;
    ui_toast("Randomizing all 3 MACs...", UI_COL_ACCENT);
    netid_randomize(); /* restarts the station link, reconnects */
    refresh();
}

static void on_restore(lv_event_t *)
{
    if (busy_guard()) return;
    netid_restore();
    ui_toast("Factory MACs restored", UI_COL_GOOD);
    refresh();
}

lv_obj_t *ui_identity_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("MAC Identity", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    lv_obj_t *intro = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM,
                               "Randomize the three radio MAC addresses to a fresh HP-OUI identity, so this board "
                               "looks like an HP device to anything that reads the vendor from the MAC. Stored and "
                               "re-applied at every boot.");
    lv_label_set_long_mode(intro, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(intro, LV_PCT(100));

    ui_caption(content, "CURRENT ADDRESSES");
    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 3, 0);
    S.v_sta = ui_kv_row(card, "Wi-Fi station");
    lv_obj_set_style_text_font(S.v_sta, &lv_font_montserrat_10, 0);
    S.v_ap = ui_kv_row(card, "Wi-Fi SoftAP");
    lv_obj_set_style_text_font(S.v_ap, &lv_font_montserrat_10, 0);
    S.v_bt = ui_kv_row(card, "Bluetooth / BLE");
    lv_obj_set_style_text_font(S.v_bt, &lv_font_montserrat_10, 0);
    S.v_label = ui_kv_row(card, "Identity");

    lv_obj_t *btns = lv_obj_create(content);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);

    lv_obj_t *rnd = ui_button(btns, "Randomize (HP)", UI_COL_ACCENT);
    lv_obj_set_flex_grow(rnd, 1);
    lv_obj_add_event_cb(rnd, on_randomize, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *fac = ui_button_outline(btns, "Factory");
    lv_obj_set_flex_grow(fac, 1);
    lv_obj_add_event_cb(fac, on_restore, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *note = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                              "Applying a new identity briefly drops and reconnects Wi-Fi.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));

    refresh();
    return lv_obj_get_screen(content);
}
