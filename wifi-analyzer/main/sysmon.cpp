#include "sysmon.h"

#include <algorithm>
#include <cstring>

#include "board.h"
#include "driver/temperature_sensor.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc.h"

static const char *TAG = "sysmon";

#define SAMPLE_MS 1000
#define BATTERY_EVERY 5 /* samples between battery reads */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sysmon_t s_cur;
static float s_hist_cpu0[SYSMON_HISTORY], s_hist_cpu1[SYSMON_HISTORY], s_hist_temp[SYSMON_HISTORY];
static int s_hist_len, s_hist_head; /* head = slot for the next sample */
static temperature_sensor_handle_t s_tsens;

/* Dynamic frequency scaling: hold a CPU_FREQ_MAX lock while the board is busy so
   it runs at the top frequency, release it when idle so esp_pm drops to the floor.
   On the ESP32-S3 the APB clock stays at 80 MHz regardless, so peripherals (the
   LCD, SPI) are unaffected by the CPU frequency moving. */
static esp_pm_lock_handle_t s_pm_lock;
static bool s_pm_held;

static void dfs_init(void)
{
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 80;
    pm.light_sleep_enable = false;
    if (esp_pm_configure(&pm) != ESP_OK) return; /* PM disabled in sdkconfig: measure only */
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "sysmon_dfs", &s_pm_lock) == ESP_OK) {
        esp_pm_lock_acquire(s_pm_lock); /* boot responsive at full speed; the governor eases off */
        s_pm_held = true;
    }
}

static void dfs_govern(float load)
{
    if (!s_pm_lock) return;
    if (!s_pm_held && load > 50) {
        esp_pm_lock_acquire(s_pm_lock);
        s_pm_held = true;
    } else if (s_pm_held && load < 20) {
        esp_pm_lock_release(s_pm_lock);
        s_pm_held = false;
    }
}

static void read_static(sysmon_t *s)
{
    esp_flash_get_size(nullptr, &s->flash_size);
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (!part) return;
    s->app_part_size = part->size;
    esp_partition_pos_t pos = {part->address, part->size};
    esp_image_metadata_t meta;
    if (esp_image_get_metadata(&pos, &meta) == ESP_OK) s->app_size = meta.image_len;
}

static void sample(sysmon_t *s, configRUN_TIME_COUNTER_TYPE prev_idle[2], int64_t *prev_t, int tick)
{
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - *prev_t);
    for (int core = 0; core < 2; core++) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);
        float busy = 100.0f * (1.0f - (float)(idle - prev_idle[core]) / dt);
        s->cpu[core] = std::clamp(busy, 0.0f, 100.0f);
        prev_idle[core] = idle;
    }
    *prev_t = now;

    float t;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) {
        if (!s->temp_ok) s->temp_min = s->temp_max = t;
        s->temp_c = t;
        s->temp_min = std::min(s->temp_min, t);
        s->temp_max = std::max(s->temp_max, t);
        s->temp_ok = true;
    }

    if (tick % BATTERY_EVERY == 0) {
        s->batt_mv = board_battery_mv();
        s->on_usb = s->batt_mv > 4300 || s->batt_mv < 2500;
        s->batt_pct = s->on_usb ? 100 : board_battery_percent(s->batt_mv);
    }

    s->heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s->heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    s->heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s->heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    s->uptime_s = (uint32_t)(now / 1000000);
    s->tasks = uxTaskGetNumberOfTasks();

    rtc_cpu_freq_config_t fc;
    rtc_clk_cpu_freq_get_config(&fc);
    s->cpu_freq_mhz = fc.freq_mhz;
}

static void sysmon_task(void *)
{
    configRUN_TIME_COUNTER_TYPE prev_idle[2] = {ulTaskGetIdleRunTimeCounterForCore(0),
                                                ulTaskGetIdleRunTimeCounterForCore(1)};
    int64_t prev_t = esp_timer_get_time();
    sysmon_t s;
    taskENTER_CRITICAL(&s_mux);
    s = s_cur;
    taskEXIT_CRITICAL(&s_mux);

    for (int tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        sample(&s, prev_idle, &prev_t, tick);
        dfs_govern(std::max(s.cpu[0], s.cpu[1]));

        taskENTER_CRITICAL(&s_mux);
        s_cur = s;
        s_hist_cpu0[s_hist_head] = s.cpu[0];
        s_hist_cpu1[s_hist_head] = s.cpu[1];
        s_hist_temp[s_hist_head] = s.temp_c;
        s_hist_head = (s_hist_head + 1) % SYSMON_HISTORY;
        s_hist_len = std::min(s_hist_len + 1, SYSMON_HISTORY);
        taskEXIT_CRITICAL(&s_mux);
    }
}

void sysmon_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = nullptr;
        ESP_LOGW(TAG, "temperature sensor unavailable");
    }

    dfs_init();

    sysmon_t s = {};
    read_static(&s);
    s.batt_mv = board_battery_mv();
    s.on_usb = s.batt_mv > 4300 || s.batt_mv < 2500;
    s.batt_pct = s.on_usb ? 100 : board_battery_percent(s.batt_mv);
    s_cur = s;

    xTaskCreatePinnedToCore(sysmon_task, "sysmon", 4096, nullptr, 2, nullptr, 0);
}

void sysmon_get(sysmon_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_cur;
    taskEXIT_CRITICAL(&s_mux);
}

int sysmon_history(float *cpu0, float *cpu1, float *temp, int max)
{
    taskENTER_CRITICAL(&s_mux);
    int n = std::min(max, s_hist_len);
    int start = (s_hist_head - n + SYSMON_HISTORY) % SYSMON_HISTORY;
    for (int i = 0; i < n; i++) {
        int k = (start + i) % SYSMON_HISTORY;
        if (cpu0) cpu0[i] = s_hist_cpu0[k];
        if (cpu1) cpu1[i] = s_hist_cpu1[k];
        if (temp) temp[i] = s_hist_temp[k];
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}
