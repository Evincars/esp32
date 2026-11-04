#include "time_sync.h"

#include <cstdlib>
#include <ctime>

#include "app_settings.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "time_sync";

typedef struct {
    const char *name;
    const char *tz; /* POSIX TZ string */
} zone_t;

static const zone_t ZONES[] = {
    {"UTC", "UTC0"},
    {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Prague / Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Helsinki / Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow", "MSK-3"},
    {"Dubai", "<+04>-4"},
    {"India", "IST-5:30"},
    {"Singapore", "<+08>-8"},
    {"Tokyo", "JST-9"},
    {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
};
#define ZONE_COUNT (int)(sizeof(ZONES) / sizeof(ZONES[0]))

static bool s_started;

void time_sync_init(void)
{
    time_sync_set_zone(g_settings.timezone);
}

void time_sync_start(void)
{
    if (s_started) return;
    s_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        s_started = false;
    }
}

bool time_sync_valid(void)
{
    return time(nullptr) > 1700000000; /* anything before late 2023 means "never set" */
}

int time_sync_zone_count(void)
{
    return ZONE_COUNT;
}

const char *time_sync_zone_name(int idx)
{
    return idx >= 0 && idx < ZONE_COUNT ? ZONES[idx].name : "?";
}

void time_sync_set_zone(int idx)
{
    if (idx < 0 || idx >= ZONE_COUNT) idx = 0;
    setenv("TZ", ZONES[idx].tz, 1);
    tzset();
}
