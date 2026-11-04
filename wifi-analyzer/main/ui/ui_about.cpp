#include <cstring>

#include "esp_app_desc.h"
#include "ui.h"

#define WEBSITE "https://lasak.netlify.app"

lv_obj_t *ui_about_create(void)
{
    lv_obj_t *content = ui_screen_create("About", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    /* author + website QR */
    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), 100);
    lv_obj_set_style_pad_all(card, 8, 0);

    lv_obj_t *qr = lv_qrcode_create(card);
    lv_qrcode_set_size(qr, 80);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, WEBSITE, strlen(WEBSITE));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 2, 0);
    lv_obj_align(qr, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *dev = ui_caption(card, "DEVELOPED BY");
    lv_obj_align(dev, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *name = ui_label(card, &lv_font_montserrat_20, UI_COL_TEXT, "Adam Lasak");
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 16);
    lv_obj_t *nick = ui_label(card, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "Evincars");
    lv_obj_align(nick, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_t *web = ui_label(card, &lv_font_montserrat_12, UI_COL_ACCENT, "lasak.netlify.app");
    lv_obj_align(web, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* app description */
    card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *t = ui_label(card, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_label_set_text_fmt(t, "WiFi Toolkit %s", app->version);
    lv_obj_t *desc = ui_label(card, &lv_font_montserrat_12, UI_COL_TEXT_DIM,
                              "Pocket network analyzer for the LilyGO T-HMI (ESP32-S3). Scans access points and "
                              "channel overlap, discovers LAN hosts, scans their TCP ports, captures and dissects "
                              "packets layer by layer, and serves all of it to a browser on the same network.");
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, LV_PCT(100));

    lv_obj_t *foot = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM,
                              "ESP-IDF + LVGL + C++   (c) 2026 Adam Lasak");
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);

    return lv_obj_get_screen(content);
}
