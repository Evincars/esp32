#include "board.h"
#include "ui.h"

#define SPLASH_MS 1400

static bool s_then_calibrate;
static bool s_force_calibrate;
static ui_screen_id_t s_then;

static void bar_exec(void *obj, int32_t v)
{
    lv_bar_set_value((lv_obj_t *)obj, v, LV_ANIM_OFF);
}

static void boot_btn_watch(lv_timer_t *)
{
    /* Holding BOOT during the splash forces touch recalibration (escape hatch for a bad calibration). */
    if (board_boot_button_pressed()) s_force_calibrate = true;
}

static void splash_done(lv_timer_t *t)
{
    lv_timer_delete((lv_timer_t *)lv_timer_get_user_data(t));
    lv_timer_delete(t);
    if (s_then_calibrate || s_force_calibrate) ui_go(UI_SCR_CALIBRATE);
    else ui_go(s_then);
}

void ui_splash_show(bool then_calibrate, ui_screen_id_t then)
{
    s_then_calibrate = then_calibrate;
    s_then = then;

    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);

    lv_obj_t *title = ui_label(scr, &lv_font_montserrat_28, UI_COL_TEXT, "EVINCARS");
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);
    ui_anim_enter(title, 100);

    lv_obj_t *sub = ui_label(scr, &lv_font_montserrat_12, UI_COL_ACCENT, "WIFI TOOLKIT  /  ESP32-S3");
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 14);
    ui_anim_enter(sub, 250);

    lv_obj_t *bar = ui_bar(scr, 140, 2);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_bar_set_range(bar, 0, 100);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, bar_exec);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, SPLASH_MS - 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_screen_load(scr);

    lv_timer_t *watch = lv_timer_create(boot_btn_watch, 50, nullptr);
    lv_timer_create(splash_done, SPLASH_MS, watch);
}
