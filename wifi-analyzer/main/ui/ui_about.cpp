#include <cstring>

#include "esp_app_desc.h"
#include "ui.h"

#define WEBSITE "https://lasak.netlify.app"

static void glow_exec(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_width((lv_obj_t *)obj, v, 0);
    lv_obj_set_style_shadow_spread((lv_obj_t *)obj, v / 8, 0);
}

static void spin_exec(void *obj, int32_t v)
{
    lv_arc_set_rotation((lv_obj_t *)obj, v);
}

static void hue_exec(void *obj, int32_t v)
{
    /* slowly cycle the avatar gradient between cyan, purple and magenta */
    lv_color_t a = lv_color_mix(UI_COL_MAGENTA, UI_COL_CYAN, (uint8_t)v);
    lv_obj_set_style_bg_grad_color((lv_obj_t *)obj, a, 0);
}

static lv_obj_t *avatar(lv_obj_t *parent)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, 84, 84);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *arc = lv_arc_create(wrap);
    lv_obj_set_size(arc, 84, 84);
    lv_obj_center(arc);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, UI_COL_MAGENTA, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 230);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, spin_exec);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, 2600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *circle = lv_obj_create(wrap);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 66, 66);
    lv_obj_center(circle);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, UI_COL_CYAN, 0);
    lv_obj_set_style_bg_grad_color(circle, UI_COL_MAGENTA, 0);
    lv_obj_set_style_bg_grad_dir(circle, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(circle, UI_COL_MAGENTA, 0);
    lv_obj_set_style_shadow_opa(circle, LV_OPA_60, 0);
    lv_obj_t *init = ui_label(circle, &lv_font_montserrat_28, lv_color_hex(0x071022), "AL");
    lv_obj_center(init);

    lv_anim_init(&a);
    lv_anim_set_var(&a, circle);
    lv_anim_set_exec_cb(&a, glow_exec);
    lv_anim_set_values(&a, 4, 26);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, circle);
    lv_anim_set_exec_cb(&a, hue_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 3000);
    lv_anim_set_playback_duration(&a, 3000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    return wrap;
}

lv_obj_t *ui_about_create(void)
{
    lv_obj_t *content = ui_screen_create("About", UI_COL_MAGENTA, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    /* author hero */
    lv_obj_t *hero = ui_card(content);
    lv_obj_set_size(hero, LV_PCT(100), 100);
    lv_obj_set_style_border_color(hero, UI_COL_MAGENTA, 0);
    lv_obj_set_style_border_opa(hero, LV_OPA_50, 0);
    lv_obj_t *av = avatar(hero);
    lv_obj_align(av, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *dev = ui_label(hero, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "DEVELOPED BY");
    lv_obj_set_style_text_letter_space(dev, 2, 0);
    lv_obj_align(dev, LV_ALIGN_TOP_LEFT, 98, 6);
    lv_obj_t *name = ui_label(hero, &lv_font_montserrat_20, UI_COL_TEXT, "Adam Lasak");
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 98, 20);
    lv_obj_t *nick = ui_label(hero, &lv_font_montserrat_14, UI_COL_MAGENTA, "aka Evincars");
    lv_obj_align(nick, LV_ALIGN_TOP_LEFT, 98, 46);
    lv_obj_t *web = ui_label(hero, &lv_font_montserrat_12, UI_COL_CYAN, LV_SYMBOL_HOME " lasak.netlify.app");
    lv_obj_align(web, LV_ALIGN_TOP_LEFT, 98, 66);
    ui_anim_enter(hero, 0);

    /* app description */
    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *t = ui_label(card, &lv_font_montserrat_16, UI_COL_CYAN, "");
    lv_label_set_text_fmt(t, LV_SYMBOL_WIFI "  WiFi Toolkit  %s", app->version);
    lv_obj_t *desc = ui_label(card, &lv_font_montserrat_12, UI_COL_TEXT,
                              "A pocket Wi-Fi analyzer for the LilyGO T-HMI (ESP32-S3). Scan nearby access points "
                              "and inspect SSID, BSSID, channel, bandwidth, signal and security; see which channels "
                              "overlap on a live graph; join networks with the on-screen keyboard; check your IP "
                              "configuration and discover other hosts on the local network.");
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, LV_PCT(100));
    ui_anim_enter(card, 80);

    /* website + QR */
    lv_obj_t *qr_card = ui_card(content);
    lv_obj_set_size(qr_card, LV_PCT(100), 112);
    lv_obj_t *qr = lv_qrcode_create(qr_card);
    lv_qrcode_set_size(qr, 92);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x071022));
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, WEBSITE, strlen(WEBSITE));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 3, 0);
    lv_obj_align(qr, LV_ALIGN_RIGHT_MID, -2, 0);

    lv_obj_t *visit = ui_label(qr_card, &lv_font_montserrat_14, UI_COL_TEXT, "Visit my website");
    lv_obj_align(visit, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_t *url = ui_label(qr_card, &lv_font_montserrat_12, UI_COL_CYAN, WEBSITE);
    lv_obj_align(url, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_t *scan = ui_label(qr_card, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "scan the code with your phone " LV_SYMBOL_RIGHT);
    lv_obj_align(scan, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_obj_t *stack = ui_label(qr_card, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "ESP-IDF  +  LVGL  +  C++");
    lv_obj_align(stack, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    ui_anim_enter(qr_card, 160);

    lv_obj_t *foot = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "(c) 2026 Adam Lasak  -  Evincars");
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);

    return lv_obj_get_screen(content);
}
