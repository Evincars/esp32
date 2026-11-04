#pragma once

#include <cstdint>

/* Packet capture with two sources:
   - LAN: every Ethernet frame the Wi-Fi station sends or receives (unicast to this board
     plus the LAN's broadcast/multicast: ARP, DHCP, mDNS, SSDP...), fully decodable.
   - Air: raw 802.11 management + data frames in monitor mode on one channel. Data of
     other stations is encrypted on WPA networks, so it decodes up to the 802.11 header.
   Captured frames go into a ring buffer in PSRAM; a capture filter decides what is kept. */

#define SNIFF_CAP 512 /* bytes kept per frame */
#define SNIFF_RING 160

typedef enum {
    SNIFF_MODE_LAN,
    SNIFF_MODE_AIR,
} sniff_mode_t;

typedef enum {
    SNIFF_SRC_RX,  /* LAN, received */
    SNIFF_SRC_TX,  /* LAN, sent by this board */
    SNIFF_SRC_AIR, /* 802.11 frame from monitor mode */
} sniff_src_t;

typedef enum {
    SF_ALL,
    SF_ARP,
    SF_ICMP,
    SF_TCP,
    SF_UDP,
    SF_DNS,
    SF_DHCP,
    SF_WEB,
    SF_MGMT,
    SF_EAPOL,
    SF_COUNT,
} sniff_filter_t;

typedef struct {
    uint32_t seq;
    uint32_t t_ms; /* since capture start */
    uint16_t len;  /* original frame length */
    uint16_t caplen;
    uint8_t src;   /* sniff_src_t */
    int8_t rssi;
    int8_t noise;
    uint8_t channel;
    uint8_t rate, sig_mode, mcs, cwb;
    uint8_t data[SNIFF_CAP];
} sniff_pkt_t;

typedef struct {
    uint32_t total;   /* everything seen, before the filter */
    uint32_t stored;  /* frames that passed the filter */
    uint32_t dropped; /* lost to a full capture lock */
    uint64_t bytes;
    uint32_t arp, icmp, tcp, udp, dns, dhcp, web, mgmt, data, eapol, other;
} sniff_stats_t;

void sniffer_init(void);
bool sniffer_start(sniff_mode_t mode);
void sniffer_stop(void);
bool sniffer_running(void);
sniff_mode_t sniffer_mode(void);

void sniffer_set_filter(sniff_filter_t f);
sniff_filter_t sniffer_filter(void);
const char *sniffer_filter_name(sniff_filter_t f);

/* Monitor-mode channel; only changeable while not connected to an AP. */
void sniffer_set_channel(int ch);
int sniffer_channel(void);

void sniffer_clear(void);
/* Sequence number of the newest stored frame (0 = none yet). */
uint32_t sniffer_last_seq(void);
/* Copies a stored frame; false once it has been overwritten. */
bool sniffer_get(uint32_t seq, sniff_pkt_t *out);
void sniffer_stats(sniff_stats_t *out);
