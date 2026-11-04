#include "board.h"
#include "ui.h"

#define SPLASH_MS 1900

static bool s_then_calibrate;
static bool s_force_calibrate;

static void arc_rotate_exec(void *obj, int32_t v)
{
    lv_arc_set_rotation((lv_obj_t *)obj, v);
}

static void glow_exec(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_width((lv_obj_t *)obj, v, 0);
    lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)(60 + v * 3), 0);
}

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
    if (s_then_calibrate || s_force_calibrate) {
        ui_go(UI_SCR_CALIBRATE);
    } else {
        lv_obj_t *menu = ui_menu_create();
        lv_screen_load_anim(menu, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, true);
    }
}

void ui_splash_show(bool then_calibrate)
{
    s_then_calibrate = then_calibrate;

    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x03050C), 0);
    lv_obj_set_style_bg_grad_color(scr, UI_COL_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    /* glowing core with a spinning gradient ring around it */
    lv_obj_t *core = lv_obj_create(scr);
    lv_obj_remove_style_all(core);
    lv_obj_set_size(core, 70, 70);
    lv_obj_align(core, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(core, UI_COL_CYAN, 0);
    lv_obj_set_style_shadow_spread(core, 2, 0);
    lv_obj_t *icon = lv_label_create(core);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, UI_COL_CYAN, 0);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_center(icon);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, core);
    lv_anim_set_exec_cb(&a, glow_exec);
    lv_anim_set_values(&a, 6, 40);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    const lv_color_t ring_cols[2] = {UI_COL_CYAN, UI_COL_MAGENTA};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *arc = lv_arc_create(scr);
        int sz = 92 + i * 16;
        lv_obj_set_size(arc, sz, sz);
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, -40);
        lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, ring_cols[i], LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_arc_set_angles(arc, 0, 100 + i * 40);

        lv_anim_init(&a);
        lv_anim_set_var(&a, arc);
        lv_anim_set_exec_cb(&a, arc_rotate_exec);
        lv_anim_set_values(&a, i ? 360 : 0, i ? 0 : 360);
        lv_anim_set_duration(&a, 1100 + i * 500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, UI_COL_TEXT, 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_label_set_text(title, "EVINCARS");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 40);
    ui_anim_enter(title, 250);

    lv_obj_t *sub = lv_label_create(scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, UI_COL_CYAN, 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_label_set_text(sub, "WIFI TOOLKIT  //  ESP32-S3");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 66);
    ui_anim_enter(sub, 450);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 160, 4);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(bar, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, UI_COL_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, UI_COL_MAGENTA, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);

    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, bar_exec);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, SPLASH_MS - 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_screen_load(scr);

    lv_timer_t *watch = lv_timer_create(boot_btn_watch, 50, nullptr);
    lv_timer_create(splash_done, SPLASH_MS, watch);
}
