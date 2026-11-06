#pragma once

#include <cstdint>

/* Samples the board once per second in its own task (CPU load per core, die
   temperature, memory, battery) and keeps a short history for charts. */

#define SYSMON_HISTORY 60

typedef struct {
    float cpu[2];   /* % busy per core over the last second */
    uint32_t cpu_freq_mhz; /* current (dynamically scaled) CPU frequency */
    float temp_c;   /* die temperature */
    float temp_min, temp_max;
    bool temp_ok;
    int batt_mv;
    int batt_pct;
    bool on_usb;    /* no battery voltage in the plausible range */
    uint32_t heap_free, heap_total, heap_min, heap_largest;
    uint32_t psram_free, psram_total;
    uint32_t flash_size, app_size, app_part_size;
    uint32_t uptime_s;
    int tasks;
} sysmon_t;

void sysmon_init(void);
void sysmon_get(sysmon_t *out);
/* Copies the history, oldest sample first. Any pointer may be null. Returns the sample count. */
int sysmon_history(float *cpu0, float *cpu1, float *temp, int max);
