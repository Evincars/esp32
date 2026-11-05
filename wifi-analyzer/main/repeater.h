#pragma once

#include <cstdint>

/* Wi-Fi repeater / range extender.

   The board stays connected to the home network as a station (WAN) and at the
   same time opens its own SoftAP (LAN). Client traffic is forwarded between the
   two interfaces and Network Address/Port Translation (lwIP NAPT) rewrites it so
   the whole LAN shares the board's single station IP on the home network - a
   custom NAT router in a box, so Wi-Fi reaches the next room.

   Throughput is measured on both sides: WAN = bytes to/from the home router,
   LAN = bytes to/from the repeater's own clients. */

#define REP_MAX_CLIENTS 10

typedef struct {
    bool running;
    bool nat_ready;        /* SoftAP up and NAT/forwarding armed */
    char ssid[33];
    bool open;             /* true = open network, false = WPA2 */
    uint8_t channel;       /* follows the station channel while upstream is up */
    bool upstream_up;      /* station associated to the home network */
    char upstream_ssid[33];
    uint32_t ap_ip;        /* gateway IP handed to clients (network byte order) */
    int clients;
    uint64_t wan_rx, wan_tx; /* bytes over the station link (to the home router) */
    uint64_t lan_rx, lan_tx; /* bytes over the SoftAP link (to the clients) */
} repeater_status_t;

typedef struct {
    uint32_t ip; /* network byte order; 0 if no DHCP lease yet */
    uint8_t mac[6];
    int8_t rssi;
} repeater_client_t;

void repeater_init(void);
/* pass == nullptr or "" opens an open network; a password of 8+ chars uses WPA2. */
bool repeater_start(const char *ssid, const char *pass);
void repeater_stop(void);
bool repeater_running(void);
void repeater_status(repeater_status_t *out);
int repeater_clients(repeater_client_t *out, int max);
