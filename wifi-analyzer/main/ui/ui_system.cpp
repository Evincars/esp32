#include <cstring>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "sysmon.h"
#include "ui.h"

#define TILE_H 42
#define CHART_H 64

static struct {
    lv_obj_t *v_cpu0, *v_cpu1, *v_temp, *v_power;
    lv_obj_t *chart;
    lv_chart_series_t *s_cpu0, *s_cpu1, *s_temp;
    lv_obj_t *ram_bar, *ram_lbl, *psram_bar, *psram_lbl, *app_bar, *app_lbl;
    lv_obj_t *v_min_heap, *v_largest, *v_tasks, *v_temp_range, *v_batt, *v_txpow, *v_uptime, *v_freq;
} S;

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on / RST";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default: return "other";
    }
}

static lv_obj_t *tile(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *t = ui_card(parent);
    lv_obj_set_size(t, LV_PCT(24), TILE_H);
    lv_obj_set_style_pad_all(t, 5, 0);
    lv_obj_t *c = ui_caption(t, caption);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, -1);
    lv_obj_t *v = ui_label(t, &lv_font_montserrat_16, UI_COL_TEXT, "-");
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 1);
    return v;
}

static lv_obj_t *usage_row(lv_obj_t *parent, lv_obj_t **lbl, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 22);
    lv_obj_t *n = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT_DIM, name);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, -2);
    *lbl = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, "");
    lv_obj_align(*lbl, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_t *bar = ui_bar(row, LV_PCT(100), 4);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    return bar;
}

static void set_usage(lv_obj_t *bar, lv_obj_t *lbl, uint32_t used, uint32_t total, bool mb)
{
    int pct = total ? (int)((uint64_t)used * 100 / total) : 0;
    lv_bar_set_value(bar, pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, pct > 90 ? UI_COL_BAD : UI_COL_ACCENT, LV_PART_INDICATOR);
    if (mb) {
        lv_label_set_text_fmt(lbl, "%lu.%lu / %lu.%lu MB  %d%%", (unsigned long)(used >> 20), (unsigned long)((used % (1 << 20)) * 10 >> 20),
                              (unsigned long)(total >> 20), (unsigned long)((total % (1 << 20)) * 10 >> 20), pct);
    } else {
        lv_label_set_text_fmt(lbl, "%lu / %lu KB  %d%%", (unsigned long)(used / 1024), (unsigned long)(total / 1024), pct);
    }
}

static void update(lv_timer_t *)
{
    sysmon_t s;
    sysmon_get(&s);

    lv_label_set_text_fmt(S.v_cpu0, "%d%%", (int)s.cpu[0]);
    lv_label_set_text_fmt(S.v_cpu1, "%d%%", (int)s.cpu[1]);
    if (S.v_freq) {
        lv_label_set_text_fmt(S.v_freq, "%lu MHz  (dynamic 80-240)", (unsigned long)s.cpu_freq_mhz);
        lv_obj_set_style_text_color(S.v_freq, s.cpu_freq_mhz >= 240 ? UI_COL_GOOD : UI_COL_TEXT, 0);
    }
    lv_obj_set_style_text_color(S.v_cpu0, s.cpu[0] > 85 ? UI_COL_BAD : UI_COL_TEXT, 0);
    lv_obj_set_style_text_color(S.v_cpu1, s.cpu[1] > 85 ? UI_COL_BAD : UI_COL_TEXT, 0);
    if (s.temp_ok) {
        lv_label_set_text_fmt(S.v_temp, "%d.%d°C", (int)s.temp_c, (int)(s.temp_c * 10) % 10);
        lv_obj_set_style_text_color(S.v_temp, s.temp_c > 70 ? UI_COL_BAD : UI_COL_TEXT, 0);
    }
    if (s.on_usb) lv_label_set_text(S.v_power, "USB");
    else lv_label_set_text_fmt(S.v_power, "%d.%02dV", s.batt_mv / 1000, s.batt_mv % 1000 / 10);

    static float c0[SYSMON_HISTORY], c1[SYSMON_HISTORY], tc[SYSMON_HISTORY];
    int n = sysmon_history(c0, c1, tc, SYSMON_HISTORY);
    int32_t *y0 = lv_chart_get_y_array(S.chart, S.s_cpu0);
    int32_t *y1 = lv_chart_get_y_array(S.chart, S.s_cpu1);
    int32_t *yt = lv_chart_get_y_array(S.chart, S.s_temp);
    for (int i = 0; i < SYSMON_HISTORY; i++) {
        int k = i - (SYSMON_HISTORY - n); /* right-align the history */
        y0[i] = k >= 0 ? (int32_t)c0[k] : LV_CHART_POINT_NONE;
        y1[i] = k >= 0 ? (int32_t)c1[k] : LV_CHART_POINT_NONE;
        yt[i] = k >= 0 ? (int32_t)tc[k] : LV_CHART_POINT_NONE;
    }
    lv_chart_refresh(S.chart);

    set_usage(S.ram_bar, S.ram_lbl, s.heap_total - s.heap_free, s.heap_total, false);
    set_usage(S.psram_bar, S.psram_lbl, s.psram_total - s.psram_free, s.psram_total, true);
    set_usage(S.app_bar, S.app_lbl, s.app_size, s.app_part_size, false);

    lv_label_set_text_fmt(S.v_min_heap, "%lu KB", (unsigned long)(s.heap_min / 1024));
    lv_label_set_text_fmt(S.v_largest, "%lu KB", (unsigned long)(s.heap_largest / 1024));
    lv_label_set_text_fmt(S.v_tasks, "%d", s.tasks);
    if (s.temp_ok) {
        lv_label_set_text_fmt(S.v_temp_range, "%d.%d - %d.%d °C", (int)s.temp_min, (int)(s.temp_min * 10) % 10,
                              (int)s.temp_max, (int)(s.temp_max * 10) % 10);
    }
    if (s.on_usb) lv_label_set_text_fmt(S.v_batt, "USB (%d.%02d V on ADC)", s.batt_mv / 1000, s.batt_mv % 1000 / 10);
    else lv_label_set_text_fmt(S.v_batt, "%d.%02d V  %d%%", s.batt_mv / 1000, s.batt_mv % 1000 / 10, s.batt_pct);
    int8_t pw;
    if (esp_wifi_get_max_tx_power(&pw) == ESP_OK) lv_label_set_text_fmt(S.v_txpow, "%d.%02d dBm", pw / 4, pw % 4 * 25);
    uint32_t up = s.uptime_s;
    lv_label_set_text_fmt(S.v_uptime, "%lud %02lu:%02lu:%02lu", (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
                          (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
}

lv_obj_t *ui_system_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("System monitor", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);

    lv_obj_t *tiles = lv_obj_create(content);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, LV_PCT(100), TILE_H);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    S.v_cpu0 = tile(tiles, "CPU 0");
    S.v_cpu1 = tile(tiles, "CPU 1");
    S.v_temp = tile(tiles, "DIE TEMP");
    S.v_power = tile(tiles, "POWER");

    /* history: CPU on the left axis (0-100 %), temperature on the right (20-80 °C) */
    lv_obj_t *cc = ui_card(content);
    lv_obj_set_size(cc, LV_PCT(100), CHART_H + 26);
    S.chart = lv_chart_create(cc);
    lv_obj_set_size(S.chart, LV_PCT(100), CHART_H);
    lv_obj_align(S.chart, LV_ALIGN_TOP_MID, 0, 0);
    lv_chart_set_type(S.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(S.chart, SYSMON_HISTORY);
    lv_chart_set_range(S.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_range(S.chart, LV_CHART_AXIS_SECONDARY_Y, 20, 80);
    lv_chart_set_div_line_count(S.chart, 3, 0);
    lv_obj_set_style_bg_opa(S.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(S.chart, 0, 0);
    lv_obj_set_style_pad_all(S.chart, 0, 0);
    lv_obj_set_style_radius(S.chart, 0, 0);
    lv_obj_set_style_line_color(S.chart, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_line_width(S.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(S.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(S.chart, 0, LV_PART_INDICATOR);
    S.s_temp = lv_chart_add_series(S.chart, UI_COL_TEXT_DIM, LV_CHART_AXIS_SECONDARY_Y);
    S.s_cpu1 = lv_chart_add_series(S.chart, UI_COL_GOOD, LV_CHART_AXIS_PRIMARY_Y);
    S.s_cpu0 = lv_chart_add_series(S.chart, UI_COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *legend = lv_obj_create(cc);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 12, 0);
    ui_label(legend, &lv_font_montserrat_10, UI_COL_ACCENT, "CPU 0 %");
    ui_label(legend, &lv_font_montserrat_10, UI_COL_GOOD, "CPU 1 %");
    ui_label(legend, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "temp 20-80 °C");
    ui_label(legend, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "last 60 s");

    lv_obj_t *mem = ui_card(content);
    lv_obj_set_size(mem, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mem, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mem, 4, 0);
    S.ram_bar = usage_row(mem, &S.ram_lbl, "Internal RAM");
    S.psram_bar = usage_row(mem, &S.psram_lbl, "PSRAM");
    S.app_bar = usage_row(mem, &S.app_lbl, "App flash");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    lv_obj_t *c = ui_card(content);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    sysmon_t s;
    sysmon_get(&s);
    lv_label_set_text_fmt(ui_kv_row(c, "Chip"), "%s rev %d.%d, %d cores @ %d MHz", CONFIG_IDF_TARGET, chip.revision / 100,
                          chip.revision % 100, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    S.v_freq = ui_kv_row(c, "CPU frequency");
    lv_label_set_text_fmt(ui_kv_row(c, "Flash / PSRAM"), "%lu MB / %lu MB", (unsigned long)(s.flash_size >> 20),
                          (unsigned long)((s.psram_total + (512 << 10)) >> 20));
    S.v_min_heap = ui_kv_row(c, "Lowest free RAM");
    S.v_largest = ui_kv_row(c, "Largest free block");
    S.v_tasks = ui_kv_row(c, "Tasks");
    S.v_temp_range = ui_kv_row(c, "Temp min / max");
    S.v_batt = ui_kv_row(c, "Battery");
    S.v_txpow = ui_kv_row(c, "Wi-Fi TX power");
    S.v_uptime = ui_kv_row(c, "Uptime");
    lv_label_set_text(ui_kv_row(c, "Last reset"), reset_reason_str(esp_reset_reason()));
    const esp_app_desc_t *app = esp_app_get_description();
    lv_label_set_text_fmt(ui_kv_row(c, "App"), "%s %s", app->project_name, app->version);
    lv_label_set_text_fmt(ui_kv_row(c, "Built"), "%s %s", app->date, app->time);
    lv_label_set_text(ui_kv_row(c, "ESP-IDF"), esp_get_idf_version());
    lv_label_set_text_fmt(ui_kv_row(c, "LVGL"), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    lv_timer_t *t = ui_screen_timer(content, update, 1000, nullptr);
    update(t);
    return lv_obj_get_screen(content);
}
