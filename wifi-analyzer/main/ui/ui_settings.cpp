#include "app_settings.h"
#include "board.h"
#include "ui.h"
#include "wifi_mgr.h"

static const uint8_t INTERVALS[] = {3, 5, 10, 30};

static lv_obj_t *s_forget_lbl;

static lv_obj_t *setting_row(lv_obj_t *parent, const char *icon, const char *title, const char *hint)
{
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, LV_PCT(100), 48);
    lv_obj_t *ic = ui_label(card, &lv_font_montserrat_16, UI_COL_BLUE, icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *t = ui_label(card, &lv_font_montserrat_14, UI_COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 26, -2);
    lv_obj_t *h = ui_label(card, &lv_font_montserrat_10, UI_COL_TEXT_DIM, hint);
    lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 26, 2);
    return card;
}

static void on_brightness(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(e);
    g_settings.brightness = lv_slider_get_value(slider);
    board_backlight_set(g_settings.brightness);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) app_settings_save();
}

static void on_autoconnect(lv_event_t *e)
{
    g_settings.autoconnect = lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
    app_settings_save();
}

static void on_interval(lv_event_t *e)
{
    g_settings.scan_interval_s = INTERVALS[lv_dropdown_get_selected(lv_event_get_current_target_obj(e))];
    app_settings_save();
}

static void on_calibrate(lv_event_t *)
{
    ui_go(UI_SCR_CALIBRATE);
}

static void update_forget_label(void)
{
    lv_label_set_text_fmt(s_forget_lbl, "%d saved network%s", wifi_mgr_known_count(),
                          wifi_mgr_known_count() == 1 ? "" : "s");
}

static void on_forget_confirm(lv_event_t *e)
{
    wifi_mgr_forget_all();
    update_forget_label();
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
    ui_toast(LV_SYMBOL_TRASH "  Saved networks erased", UI_COL_RED);
}

static void on_msgbox_cancel(lv_event_t *e)
{
    lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
}

static void on_forget(lv_event_t *)
{
    lv_obj_t *mb = lv_msgbox_create(nullptr);
    lv_obj_set_width(mb, 260);
    lv_obj_set_style_bg_color(mb, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(mb, UI_COL_RED, 0);
    lv_obj_set_style_border_width(mb, 1, 0);
    lv_msgbox_add_title(mb, "Forget networks?");
    lv_msgbox_add_text(mb, "All saved Wi-Fi passwords will be erased from this device.");
    lv_obj_t *cancel = lv_msgbox_add_footer_button(mb, "Cancel");
    lv_obj_add_event_cb(cancel, on_msgbox_cancel, LV_EVENT_CLICKED, mb);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "Erase");
    lv_obj_set_style_bg_color(ok, UI_COL_RED, 0);
    lv_obj_add_event_cb(ok, on_forget_confirm, LV_EVENT_CLICKED, mb);
}

lv_obj_t *ui_settings_create(void)
{
    lv_obj_t *content = ui_screen_create("Settings", UI_COL_BLUE, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    lv_obj_t *row = setting_row(content, LV_SYMBOL_IMAGE, "Brightness", "backlight PWM");
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 120, 8);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, g_settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COL_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 12);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_RELEASED, nullptr);
    ui_anim_enter(row, 0);

    row = setting_row(content, LV_SYMBOL_LOOP, "Auto-connect", "join last network at boot");
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, UI_COL_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (g_settings.autoconnect) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_autoconnect, LV_EVENT_VALUE_CHANGED, nullptr);
    ui_anim_enter(row, 60);

    row = setting_row(content, LV_SYMBOL_REFRESH, "Scan interval", "scanner & channel graph");
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, "3 s\n5 s\n10 s\n30 s");
    lv_obj_set_size(dd, 80, 32);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(dd, UI_COL_CARD_HI, 0);
    lv_obj_set_style_border_color(dd, UI_COL_BORDER, 0);
    for (size_t i = 0; i < sizeof(INTERVALS); i++) {
        if (INTERVALS[i] == g_settings.scan_interval_s) lv_dropdown_set_selected(dd, i);
    }
    lv_obj_add_event_cb(dd, on_interval, LV_EVENT_VALUE_CHANGED, nullptr);
    ui_anim_enter(row, 120);

    row = setting_row(content, LV_SYMBOL_GPS, "Touch calibration", "if taps land off-target");
    lv_obj_t *btn = ui_button(row, "Calibrate", UI_COL_BLUE);
    lv_obj_set_height(btn, 30);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btn, on_calibrate, LV_EVENT_CLICKED, nullptr);
    ui_anim_enter(row, 180);

    row = setting_row(content, LV_SYMBOL_TRASH, "Saved networks", "");
    s_forget_lbl = lv_obj_get_child(row, 2);
    update_forget_label();
    btn = ui_button(row, "Forget", UI_COL_RED);
    lv_obj_set_height(btn, 30);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btn, on_forget, LV_EVENT_CLICKED, nullptr);
    ui_anim_enter(row, 240);

    return lv_obj_get_screen(content);
}
