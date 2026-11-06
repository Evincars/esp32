#include "lvgl_port.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lvgl_port";

#define DRAW_BUF_LINES 32   /* internal DMA RAM is tight with Wi-Fi+BLE coexistence; smaller partial buffer */
#define LVGL_TASK_STACK (12 * 1024)
#define LVGL_TASK_PRIO 4
#define LVGL_TASK_CORE 1
#define TOUCH_MISS_TOLERANCE 3 /* noisy samples tolerated before reporting a release */

static SemaphoreHandle_t s_lock;
static esp_lcd_panel_handle_t s_panel;
static volatile lvgl_port_raw_touch_hook_t s_raw_hook;

static bool on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data)
{
    static int last_x, last_y, misses;
    static bool was_pressed;

    touch_raw_t raw;
    bool pressed = touch_read_raw(&raw);

    lvgl_port_raw_touch_hook_t hook = s_raw_hook;
    if (hook) {
        hook(pressed, &raw);
        data->state = LV_INDEV_STATE_RELEASED;
        was_pressed = false;
        return;
    }

    if (pressed && touch_calib_valid()) {
        int x, y;
        touch_map(&raw, &x, &y);
        if (was_pressed) {
            /* light smoothing; resistive panels jitter by a few pixels */
            x = (x * 2 + last_x) / 3;
            y = (y * 2 + last_y) / 3;
        }
        last_x = x;
        last_y = y;
        misses = 0;
        was_pressed = true;
    } else if (was_pressed && touch_is_down() && ++misses <= TOUCH_MISS_TOLERANCE) {
        /* still in contact, just a rejected sample: hold the previous point */
    } else {
        was_pressed = false;
    }

    data->point.x = last_x;
    data->point.y = last_y;
    data->state = was_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *)
{
    while (true) {
        uint32_t wait_ms = 10;
        if (lvgl_port_lock(portMAX_DELAY)) {
            wait_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (wait_ms < 5) wait_ms = 5;
        if (wait_ms > 30) wait_ms = 30;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

void lvgl_port_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    const size_t buf_bytes = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t);
    s_panel = board_lcd_init(buf_bytes, on_trans_done, disp);

    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(buf1 && buf2);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, nullptr, LVGL_TASK_PRIO, nullptr, LVGL_TASK_CORE);
    ESP_LOGI(TAG, "LVGL %d.%d.%d running", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

void lvgl_port_set_raw_touch_hook(lvgl_port_raw_touch_hook_t hook)
{
    s_raw_hook = hook;
}
