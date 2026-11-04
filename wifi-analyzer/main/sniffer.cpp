#include "sniffer.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "pkt_decode.h"
#include "wifi_mgr.h"

static const char *TAG = "sniffer";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sniff_pkt_t *s_ring; /* SNIFF_RING slots in PSRAM, slot = seq % SNIFF_RING */
static uint32_t s_seq;      /* last stored */
static uint32_t s_floor;    /* frames up to here were cleared */
static sniff_stats_t s_stats;
static volatile bool s_running;
static sniff_mode_t s_mode;
static volatile sniff_filter_t s_filter = SF_ALL;
static int64_t s_t0;
static int s_channel = 1;

/* LAN hook: the original lwIP entry points of the station netif */
static struct netif *s_nif;
static netif_input_fn s_orig_input;
static netif_linkoutput_fn s_orig_linkoutput;
static bool s_hooked; /* the originals are kept after unhooking: a frame may still be in flight */

static const char *FILTER_NAMES[SF_COUNT] = {"All",  "ARP",  "ICMP",      "TCP",  "UDP",
                                             "DNS",  "DHCP", "HTTP/TLS", "Mgmt", "EAPOL"};
static const uint32_t FILTER_MASK[SF_COUNT] = {
    0xFFFFFFFF, PKT_C_ARP,  PKT_C_ICMP, PKT_C_TCP,  PKT_C_UDP,
    PKT_C_DNS,  PKT_C_DHCP, PKT_C_WEB,  PKT_C_MGMT, PKT_C_EAPOL,
};

/* ---------------- ring ---------------- */

static void count(uint32_t cls, int len)
{
    s_stats.total++;
    s_stats.bytes += len;
    if (cls & PKT_C_ARP) s_stats.arp++;
    if (cls & PKT_C_ICMP) s_stats.icmp++;
    if (cls & PKT_C_TCP) s_stats.tcp++;
    if (cls & PKT_C_UDP) s_stats.udp++;
    if (cls & PKT_C_DNS) s_stats.dns++;
    if (cls & PKT_C_DHCP) s_stats.dhcp++;
    if (cls & PKT_C_WEB) s_stats.web++;
    if (cls & PKT_C_MGMT) s_stats.mgmt++;
    if (cls & PKT_C_DATA) s_stats.data++;
    if (cls & PKT_C_EAPOL) s_stats.eapol++;
    if (!(cls & (PKT_C_ARP | PKT_C_ICMP | PKT_C_TCP | PKT_C_UDP | PKT_C_MGMT | PKT_C_DATA | PKT_C_EAPOL))) {
        s_stats.other++;
    }
}

typedef struct {
    uint8_t src;
    int8_t rssi, noise;
    uint8_t channel, rate, sig_mode, mcs, cwb;
} meta_t;

/* Called from the Wi-Fi task (RX, monitor mode) and from lwIP's TX path, which lwIP
   serializes. The critical section only covers the copy into the ring. */
static void store(const meta_t *m, const uint8_t *data, int caplen, int len)
{
    uint32_t cls = pkt_classify(data, caplen, m->src == SNIFF_SRC_AIR);
    bool keep = (cls & FILTER_MASK[s_filter]) != 0;

    taskENTER_CRITICAL(&s_mux);
    count(cls, len);
    if (keep && s_ring) {
        uint32_t seq = s_seq + 1;
        sniff_pkt_t *slot = &s_ring[seq % SNIFF_RING];
        slot->seq = seq;
        slot->t_ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
        slot->len = len;
        slot->caplen = caplen;
        slot->src = m->src;
        slot->rssi = m->rssi;
        slot->noise = m->noise;
        slot->channel = m->channel;
        slot->rate = m->rate;
        slot->sig_mode = m->sig_mode;
        slot->mcs = m->mcs;
        slot->cwb = m->cwb;
        memcpy(slot->data, data, caplen);
        s_seq = seq;
        s_stats.stored++;
    }
    taskEXIT_CRITICAL(&s_mux);
}

/* Chained pbufs are flattened here first; one buffer per direction because RX runs
   in the Wi-Fi task and TX in whichever task holds lwIP. */
static uint8_t s_stage_rx[SNIFF_CAP], s_stage_tx[SNIFF_CAP];

static void capture_pbuf(struct pbuf *p, sniff_src_t src)
{
    if (!s_running || !p) return;
    uint8_t *buf = src == SNIFF_SRC_TX ? s_stage_tx : s_stage_rx;
    int caplen = pbuf_copy_partial(p, buf, std::min<int>(p->tot_len, SNIFF_CAP), 0);
    meta_t m = {};
    m.src = src;
    store(&m, buf, caplen, p->tot_len);
}

static err_t hook_input(struct pbuf *p, struct netif *inp)
{
    capture_pbuf(p, SNIFF_SRC_RX);
    return s_orig_input(p, inp);
}

static err_t hook_linkoutput(struct netif *nif, struct pbuf *p)
{
    capture_pbuf(p, SNIFF_SRC_TX);
    return s_orig_linkoutput(nif, p);
}

static esp_err_t install_hook(void *)
{
    if (s_hooked) return ESP_OK;
    if (s_nif->input != hook_input) s_orig_input = s_nif->input;
    if (s_nif->linkoutput != hook_linkoutput) s_orig_linkoutput = s_nif->linkoutput;
    s_nif->input = hook_input;
    s_nif->linkoutput = hook_linkoutput;
    s_hooked = true;
    return ESP_OK;
}

static esp_err_t remove_hook(void *)
{
    if (!s_hooked) return ESP_OK;
    /* only restore if nobody re-created the netif under us */
    if (s_nif->input == hook_input) s_nif->input = s_orig_input;
    if (s_nif->linkoutput == hook_linkoutput) s_nif->linkoutput = s_orig_linkoutput;
    s_hooked = false;
    return ESP_OK;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running || type == WIFI_PKT_MISC) return;
    auto *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (type != WIFI_PKT_CTRL) len -= 4; /* sig_len includes the FCS */
    if (len <= 0) return;

    meta_t m = {};
    m.src = SNIFF_SRC_AIR;
    m.rssi = pkt->rx_ctrl.rssi;
    m.noise = pkt->rx_ctrl.noise_floor;
    m.channel = pkt->rx_ctrl.channel;
    m.rate = pkt->rx_ctrl.rate;
    m.sig_mode = pkt->rx_ctrl.sig_mode;
    m.mcs = pkt->rx_ctrl.mcs;
    m.cwb = pkt->rx_ctrl.cwb;
    store(&m, pkt->payload, std::min(len, SNIFF_CAP), len);
}

/* ---------------- control ---------------- */

void sniffer_init(void)
{
    s_ring = (sniff_pkt_t *)heap_caps_calloc(SNIFF_RING, sizeof(sniff_pkt_t), MALLOC_CAP_SPIRAM);
    if (!s_ring) ESP_LOGE(TAG, "no memory for the capture ring");
}

bool sniffer_start(sniff_mode_t mode)
{
    if (s_running) sniffer_stop();
    if (!s_ring) return false;
    s_mode = mode;

    if (mode == SNIFF_MODE_LAN) {
        if (wifi_mgr_state() != WIFI_ST_CONNECTED) return false;
        s_nif = (struct netif *)esp_netif_get_netif_impl(wifi_mgr_netif());
        if (!s_nif) return false;
        s_t0 = esp_timer_get_time();
        s_running = true;
        esp_netif_tcpip_exec(install_hook, nullptr);
    } else {
        wifi_promiscuous_filter_t f = {};
        f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
        esp_wifi_set_promiscuous_filter(&f);
        esp_wifi_set_promiscuous_rx_cb(promisc_cb);
        s_t0 = esp_timer_get_time();
        s_running = true;
        if (esp_wifi_set_promiscuous(true) != ESP_OK) {
            s_running = false;
            return false;
        }
        if (wifi_mgr_state() != WIFI_ST_CONNECTED) esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    }
    ESP_LOGI(TAG, "capture started (%s)", mode == SNIFF_MODE_LAN ? "LAN" : "802.11");
    return true;
}

void sniffer_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_mode == SNIFF_MODE_LAN) {
        esp_netif_tcpip_exec(remove_hook, nullptr);
    } else {
        esp_wifi_set_promiscuous(false);
    }
    ESP_LOGI(TAG, "capture stopped");
}

bool sniffer_running(void)
{
    return s_running;
}

sniff_mode_t sniffer_mode(void)
{
    return s_mode;
}

void sniffer_set_filter(sniff_filter_t f)
{
    if (f < SF_COUNT) s_filter = f;
}

sniff_filter_t sniffer_filter(void)
{
    return s_filter;
}

const char *sniffer_filter_name(sniff_filter_t f)
{
    return f < SF_COUNT ? FILTER_NAMES[f] : "?";
}

void sniffer_set_channel(int ch)
{
    if (ch < 1 || ch > 13) ch = 1;
    s_channel = ch;
    if (s_running && s_mode == SNIFF_MODE_AIR && wifi_mgr_state() != WIFI_ST_CONNECTED) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
}

int sniffer_channel(void)
{
    if (wifi_mgr_state() == WIFI_ST_CONNECTED) {
        uint8_t primary;
        wifi_second_chan_t second;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK) return primary;
    }
    return s_channel;
}

void sniffer_clear(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_floor = s_seq;
    s_stats = {};
    taskEXIT_CRITICAL(&s_mux);
    s_t0 = esp_timer_get_time();
}

uint32_t sniffer_last_seq(void)
{
    return s_seq;
}

bool sniffer_get(uint32_t seq, sniff_pkt_t *out)
{
    if (!s_ring || seq <= s_floor) return false;
    taskENTER_CRITICAL(&s_mux);
    const sniff_pkt_t *slot = &s_ring[seq % SNIFF_RING];
    bool ok = slot->seq == seq;
    if (ok) {
        memcpy(out, slot, offsetof(sniff_pkt_t, data) + slot->caplen);
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void sniffer_stats(sniff_stats_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_mux);
}
