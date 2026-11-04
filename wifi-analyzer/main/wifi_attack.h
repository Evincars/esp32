#pragma once

#include <cstdint>

#include "esp_wifi.h"

/* Raw 802.11 transmit tools for authorized RF resilience testing only:
   - Deauthentication flood against one BSSID (optionally one client).
   - Beacon spam: broadcasts many fake access points on cycling channels.
   Both take over the radio (promiscuous mode) and stop any STA connection,
   so starting one stops the other and the LAN sniffer. */

#define ATTACK_MAX_BEACONS 48

typedef struct {
    bool running;
    uint32_t frames;   /* frames transmitted so far */
    uint32_t pps;      /* most recent frames-per-second */
    uint8_t channel;   /* current channel */
} attack_status_t;

void wifi_attack_init(void);

/* Deauth one access point. client == nullptr floods the broadcast address. */
bool wifi_attack_deauth_start(const uint8_t bssid[6], uint8_t channel, const uint8_t client[6]);
void wifi_attack_deauth_stop(void);
bool wifi_attack_deauth_running(void);
void wifi_attack_deauth_status(attack_status_t *out);

/* Beacon spam. `count` fake SSIDs (<= ATTACK_MAX_BEACONS) built from `prefix`. */
bool wifi_attack_beacon_start(const char *prefix, int count);
void wifi_attack_beacon_stop(void);
bool wifi_attack_beacon_running(void);
void wifi_attack_beacon_status(attack_status_t *out);
int wifi_attack_beacon_count(void);
