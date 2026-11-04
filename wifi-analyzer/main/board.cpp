#include "board.h"

#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static adc_channel_t s_adc_channel;
static bool s_backlight_ready;

void board_power_init(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_PWR_EN) | (1ULL << PIN_PWR_ON);
    io.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_PWR_EN, 1);
    gpio_set_level(PIN_PWR_ON, 1);
    /* After deep sleep the pins are still latched; release them only once the
       output registers hold the right levels so the battery latch never drops. */
    gpio_hold_dis(PIN_PWR_EN);
    gpio_hold_dis(PIN_PWR_ON);
    gpio_hold_dis(PIN_LCD_BL);
    gpio_deep_sleep_hold_dis();

    gpio_config_t btn = {};
    btn.pin_bit_mask = (1ULL << PIN_BUTTON_BOOT) | (1ULL << PIN_BUTTON_USER);
    btn.mode = GPIO_MODE_INPUT;
    btn.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&btn));
}

esp_lcd_panel_handle_t board_lcd_init(size_t max_transfer_bytes, board_lcd_trans_done_cb_t on_done, void *user_ctx)
{
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_i80_bus_config_t bus_cfg = {};
    bus_cfg.dc_gpio_num = PIN_LCD_DC;
    bus_cfg.wr_gpio_num = PIN_LCD_WR;
    bus_cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    const gpio_num_t data_pins[8] = {PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
                                     PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7};
    for (int i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        bus_cfg.data_gpio_nums[i] = i < 8 ? data_pins[i] : GPIO_NUM_NC;
    }
    bus_cfg.bus_width = 8;
    bus_cfg.max_transfer_bytes = max_transfer_bytes;
    bus_cfg.dma_burst_size = 64;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_i80_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_LCD_CS;
    io_cfg.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_done;
    io_cfg.user_ctx = user_ctx;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.dc_levels.dc_idle_level = 0;
    io_cfg.dc_levels.dc_cmd_level = 0;
    io_cfg.dc_levels.dc_dummy_level = 0;
    io_cfg.dc_levels.dc_data_level = 1;
    /* LVGL renders little-endian RGB565, the ST7789 wants big-endian over 8 bits. */
    io_cfg.flags.swap_color_bytes = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "ST7789 %dx%d @ %d MHz ready", LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLOCK_HZ / 1000000);
    return panel;
}

#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES LEDC_TIMER_10_BIT

void board_backlight_init(void)
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = BL_LEDC_MODE;
    timer.duty_resolution = BL_LEDC_RES;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {};
    ch.gpio_num = PIN_LCD_BL;
    ch.speed_mode = BL_LEDC_MODE;
    ch.channel = BL_LEDC_CHANNEL;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0; /* stays dark until the first frame is drawn */
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    s_backlight_ready = true;
}

void board_backlight_set(int percent)
{
    if (!s_backlight_ready) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Perceptual (squared) curve so the low end of the slider is usable. */
    uint32_t max = (1u << 10) - 1;
    uint32_t duty = (uint32_t)(percent * percent) * max / 10000;
    if (percent > 0 && duty < 4) duty = 4;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

void board_battery_init(void)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(PIN_BAT_ADC, &unit, &s_adc_channel));

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = unit;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t ch_cfg = {};
    ch_cfg.atten = ADC_ATTEN_DB_12;
    ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_adc_channel, &ch_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = unit;
    cali_cfg.chan = s_adc_channel;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable, battery reading will be approximate");
        s_adc_cali = nullptr;
    }
}

int board_battery_mv(void)
{
    if (!s_adc) return 0;
    int sum = 0;
    const int samples = 8;
    for (int i = 0; i < samples; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, s_adc_channel, &raw) != ESP_OK) return 0;
        if (s_adc_cali) {
            adc_cali_raw_to_voltage(s_adc_cali, raw, &mv);
        } else {
            mv = raw * 3100 / 4095;
        }
        sum += mv;
    }
    return (sum / samples) * 2;
}

int board_battery_percent(int mv)
{
    static const struct { int mv; int pct; } curve[] = {
        {4150, 100}, {4050, 90}, {3970, 80}, {3900, 70}, {3840, 60}, {3790, 50},
        {3750, 40},  {3710, 30}, {3670, 20}, {3610, 10}, {3500, 5},  {3300, 0},
    };
    if (mv >= curve[0].mv) return 100;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (mv >= curve[i].mv) {
            int span_mv = curve[i - 1].mv - curve[i].mv;
            int span_pct = curve[i - 1].pct - curve[i].pct;
            return curve[i].pct + (mv - curve[i].mv) * span_pct / span_mv;
        }
    }
    return 0;
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(PIN_BUTTON_BOOT) == 0;
}

void board_deep_sleep(void)
{
    if (s_backlight_ready) ledc_stop(BL_LEDC_MODE, BL_LEDC_CHANNEL, 0);
    gpio_config_t bl = {};
    bl.pin_bit_mask = 1ULL << PIN_LCD_BL;
    bl.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 0);
    gpio_set_level(PIN_PWR_ON, 0); /* LCD + touch */

    gpio_hold_en(PIN_LCD_BL);
    gpio_hold_en(PIN_PWR_ON);
    gpio_hold_en(PIN_PWR_EN); /* stays high: on battery it keeps the board powered */
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_ext0_wakeup(PIN_BUTTON_BOOT, 0);
    rtc_gpio_pullup_en(PIN_BUTTON_BOOT);
    rtc_gpio_pulldown_dis(PIN_BUTTON_BOOT);

    /* don't wake straight away if BOOT is still held */
    while (board_boot_button_pressed()) vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "entering deep sleep");
    esp_deep_sleep_start();
}
