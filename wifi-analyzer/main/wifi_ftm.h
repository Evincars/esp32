#pragma once

#include <cstdint>

/* Wi-Fi Fine Timing Measurement (802.11mc / RTT). If a nearby AP is an FTM
   responder, the round-trip time of a burst of measurement frames yields the
   distance to it with sub-metre resolution. The UI plots the distance (and a
   movement-based relative bearing) as a vector on a radar. */

typedef enum {
    FTM_IDLE,
    FTM_MEASURING,
    FTM_OK,
    FTM_FAIL,
} ftm_state_t;

typedef struct {
    bool running;
    ftm_state_t state;
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    uint32_t dist_cm;  /* last estimated one-way distance */
    uint32_t rtt_ns;   /* last estimated round-trip time */
    int rssi;          /* live RSSI of the target */
    uint32_t sessions; /* completed measurement sessions */
    uint32_t fails;
} ftm_status_t;

void wifi_ftm_init(void);
bool wifi_ftm_start(const uint8_t bssid[6], const char *ssid, uint8_t channel);
void wifi_ftm_stop(void);
bool wifi_ftm_running(void);
void wifi_ftm_status(ftm_status_t *out);
