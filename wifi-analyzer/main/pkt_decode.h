#pragma once

#include <cstddef>
#include <cstdint>

#include "sniffer.h"

/* Protocol dissectors for captured frames: a one-line summary for packet lists and
   a per-OSI-layer breakdown for the detail view (Wireshark style). */

/* Classes used by the capture filter and the statistics. */
enum {
    PKT_C_ARP = 1 << 0,
    PKT_C_ICMP = 1 << 1,
    PKT_C_TCP = 1 << 2,
    PKT_C_UDP = 1 << 3,
    PKT_C_DNS = 1 << 4,
    PKT_C_DHCP = 1 << 5,
    PKT_C_WEB = 1 << 6, /* HTTP / TLS / QUIC */
    PKT_C_MGMT = 1 << 7, /* 802.11 management */
    PKT_C_DATA = 1 << 8, /* 802.11 data */
    PKT_C_EAPOL = 1 << 9,
};

typedef struct {
    char proto[12];
    char src[42];
    char dst[42];
    char info[96];
    uint32_t cls;
    uint8_t top_osi; /* highest OSI layer that was decoded */
} pkt_summary_t;

#define PKT_MAX_LAYERS 7
#define PKT_MAX_FIELDS 12

typedef struct {
    char k[18];
    char v[50];
} pkt_field_t;

typedef struct {
    uint8_t osi; /* 1..7 */
    char name[30];
    uint16_t off, len; /* byte range of this layer's header in the frame */
    uint8_t nfields;
    pkt_field_t f[PKT_MAX_FIELDS];
} pkt_layer_t;

/* Class bits only, cheap enough for the capture path. */
uint32_t pkt_classify(const uint8_t *data, int caplen, bool is_80211);
void pkt_summarize(const sniff_pkt_t *p, pkt_summary_t *out);
/* Fills up to `max` layers, lowest first; returns how many. */
int pkt_dissect(const sniff_pkt_t *p, pkt_layer_t *layers, int max);
const char *pkt_osi_name(int osi);
/* Writes "off  hex bytes  ascii" lines, `per_line` bytes each. */
void pkt_hexdump(const uint8_t *d, int len, int per_line, char *out, size_t out_len);

/* Extracts a readable application payload for cleartext HTTP frames (headers and,
   if present, the body - JSON is pretty-printed). Returns bytes written (0 if the
   frame has no readable payload, e.g. it is encrypted TLS or not HTTP). `kind`, if
   given, is set to "HTTP", "JSON", "XML" or "text". */
int pkt_payload(const sniff_pkt_t *p, char *out, size_t out_len, const char **kind);
