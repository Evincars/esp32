#include <cstring>

#include "app_settings.h"
#include "board.h"
#include "boot_mode.h"
#include "lvgl_port.h"
#include "time_sync.h"
#include "ui.h"
#include "wifi_mgr.h"

static const uint8_t INTERVALS[] = {3, 5, 10, 30};

static lv_obj_t *s_forget_lbl;

static lv_obj_t *setting_row(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, LV_PCT(100), 40);
    lv_obj_set_style_pad_hor(card, 8, 0);
    lv_obj_t *t = ui_label(card, &lv_font_montserrat_14, UI_COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, -3);
    lv_obj_t *h = ui_label(card, &lv_font_montserrat_10, UI_COL_TEXT_DIM, hint);
    lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 3);
    return card;
}

static lv_obj_t *row_dropdown(lv_obj_t *row, const char *options, int selected, int width)
{
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_size(dd, width, 28);
    lv_obj_set_style_pad_ver(dd, 5, 0);
    lv_obj_set_style_radius(dd, 3, 0);
    lv_obj_set_style_bg_color(dd, UI_COL_CARD_HI, 0);
    lv_obj_set_style_border_color(dd, UI_COL_BORDER, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    return dd;
}

static lv_obj_t *row_switch(lv_obj_t *row, bool on)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 40, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static lv_obj_t *row_button(lv_obj_t *row, const char *text, lv_color_t col)
{
    lv_obj_t *btn = ui_button(row, text, col);
    lv_obj_set_size(btn, 84, 28);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(btn, 0), &lv_font_montserrat_12, 0);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    return btn;
}

static bool checked(lv_event_t *e)
{
    return lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
}

static uint32_t selected(lv_event_t *e)
{
    return lv_dropdown_get_selected(lv_event_get_current_target_obj(e));
}

static void reload_async(void *)
{
    ui_reload();
}

static void on_palette(lv_event_t *e)
{
    g_settings.palette = selected(e);
    app_settings_save();
    ui_theme_apply(g_settings.palette);
    /* rebuilding deletes this dropdown, so not from inside its own event */
    lv_async_call(reload_async, nullptr);
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
    g_settings.autoconnect = checked(e);
    app_settings_save();
}

static void on_interval(lv_event_t *e)
{
    g_settings.scan_interval_s = INTERVALS[selected(e)];
    app_settings_save();
}

static void on_timezone(lv_event_t *e)
{
    g_settings.timezone = selected(e);
    time_sync_set_zone(g_settings.timezone);
    app_settings_save();
}

static void on_web(lv_event_t *e)
{
    g_settings.web_autostart = checked(e);
    app_settings_save();
}

static void on_rst(lv_event_t *e)
{
    g_settings.rst_shortcuts = checked(e);
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

static void do_forget(void)
{
    wifi_mgr_forget_all();
    update_forget_label();
    ui_toast("Saved networks erased", UI_COL_BAD);
}

static void on_forget(lv_event_t *)
{
    ui_confirm("Forget networks?", "All saved Wi-Fi passwords will be erased from this device.", "Erase", UI_COL_BAD,
               do_forget);
}

static void sleep_now(lv_timer_t *)
{
    lvgl_port_unlock(); /* the LVGL task never runs again; don't leave its lock held */
    boot_mode_sleep();
}

static void on_sleep(lv_event_t *)
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t *l = ui_label(scr, &lv_font_montserrat_14, UI_COL_TEXT_DIM, "Sleeping - press BOOT or RST to wake");
    lv_obj_center(l);
    lv_screen_load(scr);
    lv_timer_t *t = lv_timer_create(sleep_now, 1200, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

lv_obj_t *ui_settings_create(void)
{
    lv_obj_t *content = ui_screen_create("Settings", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);

    char opts[256] = "";
    for (int i = 0; i < UI_PALETTE_COUNT; i++) {
        strlcat(opts, UI_PALETTES[i].name, sizeof(opts));
        if (i < UI_PALETTE_COUNT - 1) strlcat(opts, "\n", sizeof(opts));
    }
    lv_obj_t *row = setting_row(content, "Color palette", "dark theme variant");
    lv_obj_add_event_cb(row_dropdown(row, opts, g_settings.palette, 110), on_palette, LV_EVENT_VALUE_CHANGED, nullptr);

    row = setting_row(content, "Brightness", "backlight");
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 120, 6);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, g_settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 12);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_RELEASED, nullptr);

    row = setting_row(content, "Auto-connect", "join the last network at boot");
    lv_obj_add_event_cb(row_switch(row, g_settings.autoconnect), on_autoconnect, LV_EVENT_VALUE_CHANGED, nullptr);

    int sel = 0;
    for (size_t i = 0; i < sizeof(INTERVALS); i++) {
        if (INTERVALS[i] == g_settings.scan_interval_s) sel = i;
    }
    row = setting_row(content, "Scan interval", "scanner and channel graph");
    lv_obj_add_event_cb(row_dropdown(row, "3 s\n5 s\n10 s\n30 s", sel, 70), on_interval, LV_EVENT_VALUE_CHANGED, nullptr);

    opts[0] = 0;
    for (int i = 0; i < time_sync_zone_count(); i++) {
        strlcat(opts, time_sync_zone_name(i), sizeof(opts));
        if (i < time_sync_zone_count() - 1) strlcat(opts, "\n", sizeof(opts));
    }
    row = setting_row(content, "Time zone", "clock, synced over NTP");
    lv_obj_add_event_cb(row_dropdown(row, opts, g_settings.timezone, 130), on_timezone, LV_EVENT_VALUE_CHANGED, nullptr);

    row = setting_row(content, "Web dashboard", "start the HTTP server at boot");
    lv_obj_add_event_cb(row_switch(row, g_settings.web_autostart), on_web, LV_EVENT_VALUE_CHANGED, nullptr);

    row = setting_row(content, "RST shortcuts", "RST: next view, twice: sleep");
    lv_obj_add_event_cb(row_switch(row, g_settings.rst_shortcuts), on_rst, LV_EVENT_VALUE_CHANGED, nullptr);

    row = setting_row(content, "Touch calibration", "if taps land off-target");
    lv_obj_add_event_cb(row_button(row, "Calibrate", UI_COL_ACCENT), on_calibrate, LV_EVENT_CLICKED, nullptr);

    row = setting_row(content, "Saved networks", "");
    s_forget_lbl = lv_obj_get_child(row, 1);
    update_forget_label();
    lv_obj_add_event_cb(row_button(row, "Forget", UI_COL_BAD), on_forget, LV_EVENT_CLICKED, nullptr);

    row = setting_row(content, "Sleep", "deep sleep, BOOT or RST wakes");
    lv_obj_add_event_cb(row_button(row, "Sleep now", UI_COL_ACCENT), on_sleep, LV_EVENT_CLICKED, nullptr);

    return lv_obj_get_screen(content);
}
