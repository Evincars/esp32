#include "repeater.h"

#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "lwip/netif.h"
#include "rogue_ap.h"
#include "wifi_mgr.h"

static const char *TAG = "repeater";

static bool s_running;
static char s_ssid[33];
static bool s_open;
static esp_netif_t *s_ap;

/* ---------------- byte counters (netif input/linkoutput hooks) ----------------
   Same lightweight technique the sniffer uses: wrap the lwIP entry points of the
   station (WAN) and SoftAP (LAN) netifs and add up tot_len per direction. The
   hooks chain to whatever was installed before, so they coexist with the
   sniffer's own hook. Installed/removed inside the TCP/IP task for safety. */

static volatile uint64_t s_wan_rx, s_wan_tx, s_lan_rx, s_lan_tx;

typedef struct {
    struct netif *nif;
    netif_input_fn orig_input;
    netif_linkoutput_fn orig_output;
    volatile uint64_t *rx;
    volatile uint64_t *tx;
    bool hooked;
} counter_t;

static counter_t s_wan, s_lan;

static err_t count_input(struct netif *nif, counter_t *c, struct pbuf *p)
{
    if (p) *c->rx += p->tot_len;
    return c->orig_input(p, nif);
}
static err_t count_output(struct netif *nif, counter_t *c, struct pbuf *p)
{
    if (p) *c->tx += p->tot_len;
    return c->orig_output(nif, p);
}

static err_t wan_input(struct pbuf *p, struct netif *nif) { return count_input(nif, &s_wan, p); }
static err_t wan_output(struct netif *nif, struct pbuf *p) { return count_output(nif, &s_wan, p); }
static err_t lan_input(struct pbuf *p, struct netif *nif) { return count_input(nif, &s_lan, p); }
static err_t lan_output(struct netif *nif, struct pbuf *p) { return count_output(nif, &s_lan, p); }

static esp_err_t hooks_install(void *)
{
    struct netif *wan = (struct netif *)esp_netif_get_netif_impl(wifi_mgr_netif());
    struct netif *lan = (struct netif *)esp_netif_get_netif_impl(s_ap);
    if (wan && wan->input != wan_input) {
        s_wan = {wan, wan->input, wan->linkoutput, &s_wan_rx, &s_wan_tx, true};
        wan->input = wan_input;
        wan->linkoutput = wan_output;
    }
    if (lan && lan->input != lan_input) {
        s_lan = {lan, lan->input, lan->linkoutput, &s_lan_rx, &s_lan_tx, true};
        lan->input = lan_input;
        lan->linkoutput = lan_output;
    }
    return ESP_OK;
}

static esp_err_t hooks_remove(void *)
{
    if (s_wan.hooked && s_wan.nif->input == wan_input) {
        s_wan.nif->input = s_wan.orig_input;
        s_wan.nif->linkoutput = s_wan.orig_output;
    }
    if (s_lan.hooked && s_lan.nif->input == lan_input) {
        s_lan.nif->input = s_lan.orig_input;
        s_lan.nif->linkoutput = s_lan.orig_output;
    }
    s_wan.hooked = s_lan.hooked = false;
    return ESP_OK;
}

/* ---------------- DNS handed to clients ---------------- */

static void offer_dns(void)
{
    /* Prefer the upstream's DNS; fall back to a public resolver. NAPT forwards it. */
    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(wifi_mgr_netif(), ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.u_addr.ip4.addr == 0) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = esp_netif_htonl(0x08080808); /* 8.8.8.8 */
    }
    esp_netif_dhcps_stop(s_ap);
    uint8_t enable = 1;
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &enable, sizeof(enable));
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(s_ap);
}

/* ---------------- lifecycle ---------------- */

bool repeater_start(const char *ssid, const char *pass)
{
    if (s_running) return false;
    if (rogue_ap_running()) rogue_ap_stop(); /* both own the SoftAP radio */

    strlcpy(s_ssid, ssid && ssid[0] ? ssid : "repeater-esp32-s3", sizeof(s_ssid));
    int plen = pass ? (int)strlen(pass) : 0;
    s_open = plen < 8;

    s_ap = wifi_mgr_ap_netif();
    s_wan_rx = 0;
    s_wan_tx = 0;
    s_lan_rx = 0;
    s_lan_tx = 0;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return false;

    /* match the station channel so the single radio does not have to hop */
    uint8_t channel = 1;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) channel = ap_info.primary;

    wifi_config_t ap = {};
    int sl = (int)strlen(s_ssid);
    if (sl > 32) sl = 32;
    memcpy(ap.ap.ssid, s_ssid, sl);
    ap.ap.ssid_len = sl;
    ap.ap.channel = channel;
    ap.ap.max_connection = REP_MAX_CLIENTS;
    ap.ap.authmode = s_open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    if (!s_open) strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;

    offer_dns();

    /* custom NAT: rewrite client traffic onto the station's single IP */
    esp_err_t nerr = esp_netif_napt_enable(s_ap);
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "napt_enable: %s", esp_err_to_name(nerr));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }

    esp_netif_tcpip_exec(hooks_install, nullptr);
    s_running = true;
    ESP_LOGW(TAG, "repeater '%s' up on ch %d (%s)", s_ssid, channel, s_open ? "open" : "WPA2");
    return true;
}

void repeater_stop(void)
{
    if (!s_running) return;
    s_running = false;
    esp_netif_tcpip_exec(hooks_remove, nullptr);
    esp_netif_napt_disable(s_ap);
    esp_wifi_set_mode(WIFI_MODE_STA); /* keeps the station link up */
    ESP_LOGI(TAG, "repeater stopped");
}

bool repeater_running(void) { return s_running; }

void repeater_status(repeater_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->running = s_running;
    strlcpy(out->ssid, s_ssid, sizeof(out->ssid));
    out->open = s_open;
    out->wan_rx = s_wan_rx;
    out->wan_tx = s_wan_tx;
    out->lan_rx = s_lan_rx;
    out->lan_tx = s_lan_tx;

    wifi_conn_info_t info;
    if (wifi_mgr_conn_info(&info)) {
        out->upstream_up = true;
        strlcpy(out->upstream_ssid, info.ssid, sizeof(out->upstream_ssid));
        out->channel = info.channel;
    }
    if (s_running) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap, &ip) == ESP_OK) out->ap_ip = ip.ip.addr;
        wifi_sta_list_t sta;
        if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK) out->clients = sta.num;
    }
}

int repeater_clients(repeater_client_t *out, int max)
{
    if (!s_running) return 0;
    wifi_sta_list_t sta;
    if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK) return 0;
    wifi_sta_mac_ip_list_t ipl;
    bool have_ip = esp_wifi_ap_get_sta_list_with_ip(&sta, &ipl) == ESP_OK;

    int n = sta.num < max ? sta.num : max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].mac, sta.sta[i].mac, 6);
        out[i].rssi = sta.sta[i].rssi;
        out[i].ip = 0;
        if (have_ip) {
            for (int j = 0; j < ipl.num; j++) {
                if (memcmp(ipl.sta[j].mac, sta.sta[i].mac, 6) == 0) {
                    out[i].ip = ipl.sta[j].ip.addr;
                    break;
                }
            }
        }
    }
    return n;
}

void repeater_init(void) {}
