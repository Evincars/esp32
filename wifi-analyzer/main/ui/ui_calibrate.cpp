#include <cstdlib>
#include <cstring>

#include "lvgl_port.h"
#include "touch.h"
#include "ui.h"

#define SKIP_SAMPLES 3 /* first readings after contact are unreliable */
#define MIN_SAMPLES 5
#define VERIFY_TOLERANCE_PX 16

/* Spread out and non-collinear, a bit inside the edges where resistive panels are worst. */
static const int TARGET_X[4] = {32, 288, 160, 160};
static const int TARGET_Y[4] = {28, 120, 214, 120}; /* [3] = verification point */

static struct {
    lv_obj_t *target;
    lv_obj_t *title;
    lv_obj_t *hint;
    int step; /* 0..2 collect, 3 verify */
    touch_raw_t raw[3];
    touch_calib_t cal;

    /* written from the raw touch hook (same LVGL task) */
    int n, skipped;
    long sum_x, sum_y;
    bool was_pressed;
    bool sample_ready;
    touch_raw_t sample;
} S;

static void raw_hook(bool pressed, const touch_raw_t *raw)
{
    if (pressed) {
        if (S.skipped < SKIP_SAMPLES) {
            S.skipped++;
        } else {
            S.sum_x += raw->x;
            S.sum_y += raw->y;
            S.n++;
        }
        S.was_pressed = true;
        if (S.target) lv_obj_set_style_bg_opa(S.target, LV_OPA_80, 0);
    } else if (S.was_pressed) {
        S.was_pressed = false;
        if (S.n >= MIN_SAMPLES) {
            S.sample.x = S.sum_x / S.n;
            S.sample.y = S.sum_y / S.n;
            S.sample_ready = true;
        }
        S.n = S.skipped = 0;
        S.sum_x = S.sum_y = 0;
        if (S.target) lv_obj_set_style_bg_opa(S.target, LV_OPA_20, 0);
    }
}

static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_shadow_width((lv_obj_t *)obj, v, 0);
}

static void show_target(void)
{
    lv_obj_set_pos(S.target, TARGET_X[S.step] - 15, TARGET_Y[S.step] - 15);
    lv_color_t col = S.step == 3 ? UI_COL_GOOD : UI_COL_ACCENT;
    lv_obj_set_style_border_color(S.target, col, 0);
    lv_obj_set_style_shadow_color(S.target, col, 0);
    lv_obj_set_style_bg_color(S.target, col, 0);
    for (int i = 0; i < 2; i++) lv_obj_set_style_bg_color(lv_obj_get_child(S.target, i), col, 0);

    if (S.step < 3) {
        lv_label_set_text_fmt(S.hint, "Tap the center of the target  (%d / 3)", S.step + 1);
    } else {
        lv_label_set_text(S.hint, "Now tap the green target to verify");
    }
    /* keep the instructions away from the active target */
    lv_obj_align(S.title, LV_ALIGN_CENTER, 0, S.step == 1 || S.step == 3 ? -60 : -16);
    lv_obj_align_to(S.hint, S.title, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
}

static void finish(void)
{
    lvgl_port_set_raw_touch_hook(nullptr);
    ui_toast("Touch calibrated", UI_COL_GOOD);
    ui_go(UI_SCR_MENU);
}

static void poll(lv_timer_t *)
{
    if (!S.sample_ready) return;
    S.sample_ready = false;

    if (S.step < 3) {
        S.raw[S.step] = S.sample;
        S.step++;
        if (S.step == 3) {
            if (!touch_calib_compute(TARGET_X, TARGET_Y, S.raw, &S.cal)) {
                S.step = 0;
                lv_label_set_text(S.title, "Hmm, that didn't work - again");
            } else {
                touch_calib_set(&S.cal, false); /* applied for the check, saved only if it passes */
            }
        }
        show_target();
        return;
    }

    int x, y;
    touch_map(&S.sample, &x, &y);
    if (abs(x - TARGET_X[3]) <= VERIFY_TOLERANCE_PX && abs(y - TARGET_Y[3]) <= VERIFY_TOLERANCE_PX) {
        touch_calib_set(&S.cal, true);
        finish();
    } else {
        S.step = 0;
        lv_label_set_text_fmt(S.title, "Off by %d px - let's retry", abs(x - TARGET_X[3]) + abs(y - TARGET_Y[3]));
        show_target();
    }
}

static void on_delete(lv_event_t *)
{
    lvgl_port_set_raw_touch_hook(nullptr);
    S.target = nullptr;
}

lv_obj_t *ui_calibrate_create(void)
{
    memset(&S, 0, sizeof(S));

    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, nullptr);

    S.title = ui_label(scr, &lv_font_montserrat_20, UI_COL_TEXT, "Touch calibration");
    S.hint = ui_label(scr, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_obj_t *tip = ui_label(scr, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "a stylus or fingernail works best");
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -40);

    S.target = lv_obj_create(scr);
    lv_obj_remove_style_all(S.target);
    lv_obj_set_size(S.target, 30, 30);
    lv_obj_set_style_radius(S.target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(S.target, 2, 0);
    lv_obj_set_style_bg_opa(S.target, LV_OPA_20, 0);
    lv_obj_set_style_shadow_opa(S.target, LV_OPA_70, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *line = lv_obj_create(S.target);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, i ? 2 : 40, i ? 40 : 2);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_center(line);
    }
    lv_obj_remove_flag(S.target, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, S.target);
    lv_anim_set_exec_cb(&a, pulse_exec);
    lv_anim_set_values(&a, 0, 22);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    show_target();
    lvgl_port_set_raw_touch_hook(raw_hook);
    ui_screen_timer(scr, poll, 30, nullptr);
    return scr;
}
