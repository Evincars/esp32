#include <cstring>

#include "board.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "ui.h"

static temperature_sensor_handle_t s_tsens;

static struct {
    lv_obj_t *ram_bar, *ram_lbl;
    lv_obj_t *psram_bar, *psram_lbl;
    lv_obj_t *v_min_heap, *v_temp, *v_batt, *v_uptime;
} S;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
    }
}

static lv_obj_t *section(lv_obj_t *parent, const char *title, lv_color_t col)
{
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_style_border_color(card, col, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
    ui_label(card, &lv_font_montserrat_12, col, title);
    return card;
}

static lv_obj_t *usage_bar(lv_obj_t *parent, lv_obj_t **lbl, const char *name, lv_color_t col)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 26);
    *lbl = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, name);
    lv_obj_align(*lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, LV_PCT(100), 6);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, col, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, UI_COL_MAGENTA, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 600, 0);
    return bar;
}

static void update(lv_timer_t *)
{
    size_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    lv_bar_set_value(S.ram_bar, ram_total ? (int)((ram_total - ram_free) * 100 / ram_total) : 0, LV_ANIM_ON);
    lv_label_set_text_fmt(S.ram_lbl, "Internal RAM   %u / %u KB used", (unsigned)((ram_total - ram_free) / 1024),
                          (unsigned)(ram_total / 1024));
    lv_bar_set_value(S.psram_bar, ps_total ? (int)((ps_total - ps_free) * 100 / ps_total) : 0, LV_ANIM_ON);
    lv_label_set_text_fmt(S.psram_lbl, "PSRAM   %u / %u KB used", (unsigned)((ps_total - ps_free) / 1024),
                          (unsigned)(ps_total / 1024));
    lv_label_set_text_fmt(S.v_min_heap, "%u KB", (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));

    float temp;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &temp) == ESP_OK) {
        lv_label_set_text_fmt(S.v_temp, "%d.%d °C", (int)temp, (int)(temp * 10) % 10);
    }

    int mv = board_battery_mv();
    if (mv > 4300 || mv < 2500) lv_label_set_text_fmt(S.v_batt, "USB power (%d.%02d V)", mv / 1000, mv % 1000 / 10);
    else lv_label_set_text_fmt(S.v_batt, "%d.%02d V  (%d%%)", mv / 1000, mv % 1000 / 10, board_battery_percent(mv));

    int64_t s = esp_timer_get_time() / 1000000;
    lv_label_set_text_fmt(S.v_uptime, "%dd %02d:%02d:%02d", (int)(s / 86400), (int)(s / 3600 % 24), (int)(s / 60 % 60),
                          (int)(s % 60));
}

lv_obj_t *ui_system_create(void)
{
    memset(&S, 0, sizeof(S));
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);
        else s_tsens = nullptr;
    }

    lv_obj_t *content = ui_screen_create("System", UI_COL_AMBER, true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);

    lv_obj_t *c = section(content, LV_SYMBOL_CHARGE "  CHIP", UI_COL_AMBER);
    lv_label_set_text_fmt(ui_kv_row(c, "Model"), "%s rev v%d.%d", CONFIG_IDF_TARGET, chip.revision / 100,
                          chip.revision % 100);
    lv_label_set_text_fmt(ui_kv_row(c, "CPU"), "%d cores @ %d MHz", chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    lv_label_set_text_fmt(ui_kv_row(c, "Radio"), "%s%s", (chip.features & CHIP_FEATURE_WIFI_BGN) ? "Wi-Fi b/g/n " : "",
                          (chip.features & CHIP_FEATURE_BLE) ? "+ BLE 5" : "");
    lv_label_set_text_fmt(ui_kv_row(c, "Flash"), "%u MB", (unsigned)(flash_size / (1024 * 1024)));
    S.v_temp = ui_kv_row(c, "Die temperature");
    ui_anim_enter(c, 0);

    c = section(content, LV_SYMBOL_SD_CARD "  MEMORY", UI_COL_CYAN);
    S.ram_bar = usage_bar(c, &S.ram_lbl, "Internal RAM", UI_COL_CYAN);
    S.psram_bar = usage_bar(c, &S.psram_lbl, "PSRAM", UI_COL_PURPLE);
    S.v_min_heap = ui_kv_row(c, "Lowest free RAM");
    ui_anim_enter(c, 70);

    c = section(content, LV_SYMBOL_BATTERY_FULL "  POWER & FIRMWARE", UI_COL_GREEN);
    S.v_batt = ui_kv_row(c, "Battery");
    S.v_uptime = ui_kv_row(c, "Uptime");
    lv_label_set_text(ui_kv_row(c, "Last reset"), reset_reason_str(esp_reset_reason()));
    const esp_app_desc_t *app = esp_app_get_description();
    lv_label_set_text_fmt(ui_kv_row(c, "App"), "%s %s", app->project_name, app->version);
    lv_label_set_text_fmt(ui_kv_row(c, "Built"), "%s %s", app->date, app->time);
    lv_label_set_text(ui_kv_row(c, "ESP-IDF"), esp_get_idf_version());
    lv_label_set_text_fmt(ui_kv_row(c, "LVGL"), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    ui_anim_enter(c, 140);

    lv_timer_t *t = ui_screen_timer(content, update, 1000, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
