#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* LilyGO T-HMI pinout (from Xinyuan-LilyGO/T-HMI examples/factory/pins.h). */
#define PIN_PWR_EN GPIO_NUM_10 /* keeps the board alive on battery */
#define PIN_PWR_ON GPIO_NUM_14 /* peripheral (LCD/touch) power */
#define PIN_BAT_ADC GPIO_NUM_5
#define PIN_BUTTON_BOOT GPIO_NUM_0
#define PIN_BUTTON_USER GPIO_NUM_21

#define PIN_LCD_D0 GPIO_NUM_48
#define PIN_LCD_D1 GPIO_NUM_47
#define PIN_LCD_D2 GPIO_NUM_39
#define PIN_LCD_D3 GPIO_NUM_40
#define PIN_LCD_D4 GPIO_NUM_41
#define PIN_LCD_D5 GPIO_NUM_42
#define PIN_LCD_D6 GPIO_NUM_45
#define PIN_LCD_D7 GPIO_NUM_46
#define PIN_LCD_WR GPIO_NUM_8
#define PIN_LCD_CS GPIO_NUM_6
#define PIN_LCD_DC GPIO_NUM_7
#define PIN_LCD_BL GPIO_NUM_38

#define PIN_TOUCH_SCLK GPIO_NUM_1
#define PIN_TOUCH_MISO GPIO_NUM_4
#define PIN_TOUCH_MOSI GPIO_NUM_3
#define PIN_TOUCH_CS GPIO_NUM_2
#define PIN_TOUCH_IRQ GPIO_NUM_9

/* Landscape: the 240x320 ST7789 is rotated with swap_xy. */
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)

/* Flip these if the image comes out mirrored or with inverted colors. */
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y true
#define LCD_INVERT_COLOR false

typedef bool (*board_lcd_trans_done_cb_t)(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx);

void board_power_init(void);
esp_lcd_panel_handle_t board_lcd_init(size_t max_transfer_bytes, board_lcd_trans_done_cb_t on_done, void *user_ctx);

void board_backlight_init(void);
/* 0..100 % */
void board_backlight_set(int percent);

void board_battery_init(void);
/* Battery voltage in millivolts (board has a 1:2 divider). */
int board_battery_mv(void);
/* 0..100, rough Li-ion curve. */
int board_battery_percent(int mv);

bool board_boot_button_pressed(void);
bool board_user_button_pressed(void);

/* Light "hibernate": backlight and panel off, CPU and radio keep running.
   board_display_wake() restores the panel and the given brightness. */
void board_display_sleep(void);
void board_display_wake(int brightness_percent);
bool board_display_asleep(void);

/* Display and touch power off, then deep sleep until BOOT is pressed (RST also wakes). */
[[noreturn]] void board_deep_sleep(void);
