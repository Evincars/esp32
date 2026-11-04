#pragma once

#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "touch.h"

/* Creates the LVGL display + touch input device and starts the render task. */
void lvgl_port_init(void);

/* Every LVGL call made outside the LVGL task must be wrapped in lock/unlock. */
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

/* While a hook is set, raw touch samples go to it and LVGL sees no presses
   (used by the calibration screen, which must work without a calibration). */
typedef void (*lvgl_port_raw_touch_hook_t)(bool pressed, const touch_raw_t *raw);
void lvgl_port_set_raw_touch_hook(lvgl_port_raw_touch_hook_t hook);
