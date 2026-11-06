#include "wifi_mgr.h"

#include <algorithm>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "nvs.h"
#include "time_sync.h"

static const char *TAG = "wifi_mgr";

#define CONNECT_RETRIES 2
#define SCAN_TIMEOUT_US (12 * 1000000) /* a cancelled scan may never report SCAN_DONE */
#define NVS_NS "wifi_known"
#define NVS_KEY "list"

/* ARP sweep: lwIP's ARP table only has 10 slots, so probe in small batches. */
#define ARP_BATCH 6
#define ARP_WAIT_MS 250

typedef struct {
    char ssid[33];
    char pass[65];
} known_net_t;

static SemaphoreHandle_t s_mutex;
static esp_netif_t *s_netif;
static esp_netif_t *s_ap_netif;

static wifi_ap_record_t s_aps[WIFI_MAX_APS];
static int s_ap_count;
static volatile bool s_scanning;
static int64_t s_scan_started_us;
static volatile uint32_t s_scan_gen;

static volatile wifi_state_t s_state = WIFI_ST_IDLE;
static char s_status_text[64] = "Not connected";
static int s_retries;
static bool s_has_pending;
static known_net_t s_pending; /* applied once the current link is torn down */
static known_net_t s_target;  /* network we are connecting to, saved on success */
static int64_t s_connected_since;

static known_net_t s_known[WIFI_MAX_KNOWN];
static int s_known_count;

static lan_host_t s_hosts[LAN_MAX_HOSTS];
static int s_host_count;
static volatile bool s_lan_scanning;
static volatile int s_lan_progress;
static volatile uint32_t s_lan_gen;

struct Lock {
    Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mutex); }
};

/* ---------------- saved networks ---------------- */

static void known_load(void)
{
    nvs_handle_t h;
    s_known_count = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_known);
    if (nvs_get_blob(h, NVS_KEY, s_known, &len) == ESP_OK) {
        s_known_count = len / sizeof(known_net_t);
    }
    nvs_close(h);
}

static void known_store(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_known_count > 0) {
        nvs_set_blob(h, NVS_KEY, s_known, s_known_count * sizeof(known_net_t));
    } else {
        nvs_erase_key(h, NVS_KEY);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* Moves (or inserts) the network to the front: index 0 is the most recently used. */
static void known_remember(const known_net_t *net)
{
    int idx = -1;
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known[i].ssid, net->ssid) == 0) idx = i;
    }
    if (idx < 0) idx = std::min(s_known_count, WIFI_MAX_KNOWN - 1);
    else if (idx == 0 && strcmp(s_known[0].pass, net->pass) == 0) return;
    memmove(&s_known[1], &s_known[0], idx * sizeof(known_net_t));
    s_known[0] = *net;
    s_known_count = std::max(s_known_count, idx + 1);
    known_store();
}

/* ---------------- connection ---------------- */

static void set_status(wifi_state_t st, const char *text)
{
    s_state = st;
    strlcpy(s_status_text, text, sizeof(s_status_text));
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Wrong password?";
    case WIFI_REASON_AUTH_FAIL:
        return "Authentication failed";
    case WIFI_REASON_NO_AP_FOUND:
        return "Network not found";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "Unsupported security";
    case WIFI_REASON_ASSOC_FAIL:
        return "Association failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Signal lost";
    default:
        return "Connection failed";
    }
}

static bool reason_is_fatal(uint8_t reason)
{
    /* No point retrying a bad password; it only delays the error message. */
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_MIC_FAILURE;
}

static void start_connect(const known_net_t *net)
{
    wifi_config_t cfg = {};
    strlcpy((char *)cfg.sta.ssid, net->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, net->pass, sizeof(cfg.sta.password));
    /* Accept anything the AP offers, including WEP/WPA1 and open networks. */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    s_target = *net;
    s_retries = 0;
    char text[64];
    snprintf(text, sizeof(text), "Connecting to %s", net->ssid);
    set_status(WIFI_ST_CONNECTING, text);

    if (s_scanning) {
        esp_wifi_scan_stop();
        s_scanning = false;
    }
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        set_status(WIFI_ST_FAILED, "Connection failed");
    }
}

static void on_wifi_event(void *, esp_event_base_t, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t n = WIFI_MAX_APS;
        static wifi_ap_record_t tmp[WIFI_MAX_APS];
        if (esp_wifi_scan_get_ap_records(&n, tmp) != ESP_OK) n = 0;
        {
            Lock l;
            memcpy(s_aps, tmp, n * sizeof(wifi_ap_record_t));
            s_ap_count = n;
        }
        s_scanning = false;
        s_scan_gen = s_scan_gen + 1;
        ESP_LOGI(TAG, "scan done: %u APs", n);
        break;
    }
    case WIFI_EVENT_STA_CONNECTED:
        esp_netif_create_ip6_linklocal(s_netif);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        auto *ev = (wifi_event_sta_disconnected_t *)data;
        Lock l;
        ESP_LOGI(TAG, "disconnected, reason %d", ev->reason);
        if (s_has_pending) {
            s_has_pending = false;
            start_connect(&s_pending);
        } else if (s_state == WIFI_ST_CONNECTING) {
            if (!reason_is_fatal(ev->reason) && s_retries < CONNECT_RETRIES) {
                s_retries++;
                esp_wifi_connect();
            } else {
                set_status(WIFI_ST_FAILED, reason_text(ev->reason));
            }
        } else if (s_state == WIFI_ST_CONNECTED) {
            /* link dropped on its own: try to get it back */
            start_connect(&s_target);
        }
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *, esp_event_base_t, int32_t id, void *)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        Lock l;
        char text[64];
        snprintf(text, sizeof(text), "Connected to %s", s_target.ssid);
        set_status(WIFI_ST_CONNECTED, text);
        s_connected_since = esp_timer_get_time();
        known_remember(&s_target);
        time_sync_start();
    }
}

void wifi_mgr_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    known_load();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi started, %d saved network(s)", s_known_count);
}

esp_netif_t *wifi_mgr_netif(void)
{
    return s_netif;
}

esp_netif_t *wifi_mgr_ap_netif(void)
{
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    return s_ap_netif;
}

bool wifi_mgr_apply_mac(const uint8_t sta[6], const uint8_t ap[6])
{
    known_net_t target;
    bool was_connected;
    {
        Lock l;
        was_connected = s_state == WIFI_ST_CONNECTED || s_state == WIFI_ST_CONNECTING;
        target = s_target;
        /* park the state machine so the stop/start disconnect events are ignored */
        s_has_pending = false;
        set_status(WIFI_ST_IDLE, "Reapplying MAC");
    }

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* both interfaces must exist and be stopped to set their MACs */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t e1 = esp_wifi_set_mac(WIFI_IF_STA, sta);
    esp_err_t e2 = esp_wifi_set_mac(WIFI_IF_AP, ap);
    esp_wifi_set_mode(mode == WIFI_MODE_NULL ? WIFI_MODE_STA : mode);
    esp_wifi_start();

    if (e1 != ESP_OK || e2 != ESP_OK)
        ESP_LOGW(TAG, "set_mac sta=%s ap=%s", esp_err_to_name(e1), esp_err_to_name(e2));
    if (was_connected && target.ssid[0]) {
        Lock l;
        start_connect(&target);
    }
    return e1 == ESP_OK && e2 == ESP_OK;
}

bool wifi_mgr_scan_start(void)
{
    if (wifi_mgr_scanning() || s_state == WIFI_ST_CONNECTING) return false;
    wifi_scan_config_t sc = {};
    sc.show_hidden = true;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 60;
    sc.scan_time.active.max = 120;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return false;
    s_scan_started_us = esp_timer_get_time();
    s_scanning = true;
    return true;
}

bool wifi_mgr_scanning(void)
{
    if (s_scanning && esp_timer_get_time() - s_scan_started_us > SCAN_TIMEOUT_US) s_scanning = false;
    return s_scanning;
}

uint32_t wifi_mgr_scan_gen(void)
{
    return s_scan_gen;
}

int wifi_mgr_get_aps(wifi_ap_record_t *out, int max)
{
    Lock l;
    int n = std::min(max, s_ap_count);
    memcpy(out, s_aps, n * sizeof(wifi_ap_record_t));
    return n;
}

int wifi_mgr_ap_count(void)
{
    return s_ap_count;
}

void wifi_mgr_connect(const char *ssid, const char *password)
{
    known_net_t net = {};
    strlcpy(net.ssid, ssid, sizeof(net.ssid));
    strlcpy(net.pass, password ? password : "", sizeof(net.pass));

    Lock l;
    if (s_state == WIFI_ST_CONNECTED || s_state == WIFI_ST_CONNECTING) {
        /* finish tearing down the old link first; the DISCONNECTED event picks this up */
        s_pending = net;
        s_has_pending = true;
        char text[64];
        snprintf(text, sizeof(text), "Connecting to %s", net.ssid);
        set_status(WIFI_ST_CONNECTING, text);
        if (esp_wifi_disconnect() != ESP_OK) {
            s_has_pending = false;
            start_connect(&net);
        }
    } else {
        start_connect(&net);
    }
}

void wifi_mgr_disconnect(void)
{
    {
        Lock l;
        s_has_pending = false;
        set_status(WIFI_ST_IDLE, "Not connected");
    }
    esp_wifi_disconnect();
}

wifi_state_t wifi_mgr_state(void)
{
    return s_state;
}

const char *wifi_mgr_state_text(void)
{
    static char copy[64];
    Lock l;
    strlcpy(copy, s_status_text, sizeof(copy));
    return copy;
}

bool wifi_mgr_conn_info(wifi_conn_info_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_wifi_get_mac(WIFI_IF_STA, out->mac);
    const char *host = nullptr;
    if (esp_netif_get_hostname(s_netif, &host) == ESP_OK && host) {
        strlcpy(out->hostname, host, sizeof(out->hostname));
    }
    if (s_state != WIFI_ST_CONNECTED) return false;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
    memcpy(out->bssid, ap.bssid, 6);
    out->rssi = ap.rssi;
    out->channel = ap.primary;
    out->second = ap.second;
    out->authmode = ap.authmode;
    out->phy_11b = ap.phy_11b;
    out->phy_11g = ap.phy_11g;
    out->phy_11n = ap.phy_11n;
    out->phy_11ax = ap.phy_11ax;

    esp_netif_get_ip_info(s_netif, &out->ip);
    for (int i = 0; i < 2; i++) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_netif, i == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            out->dns[i] = dns.ip.u_addr.ip4;
        }
    }
    out->has_ip6 = esp_netif_get_ip6_linklocal(s_netif, &out->ip6) == ESP_OK;
    out->connected_since_us = s_connected_since;
    return true;
}

bool wifi_mgr_target_ssid(char *ssid, size_t len)
{
    Lock l;
    strlcpy(ssid, s_target.ssid, len);
    return s_target.ssid[0] != 0;
}

bool wifi_mgr_autoconnect(void)
{
    known_net_t net;
    {
        Lock l;
        if (s_known_count == 0) return false;
        net = s_known[0];
    }
    wifi_mgr_connect(net.ssid, net.pass);
    return true;
}

bool wifi_mgr_known_password(const char *ssid, char *pass_out, size_t len)
{
    Lock l;
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) {
            strlcpy(pass_out, s_known[i].pass, len);
            return true;
        }
    }
    return false;
}

int wifi_mgr_known_count(void)
{
    return s_known_count;
}

void wifi_mgr_forget_all(void)
{
    Lock l;
    s_known_count = 0;
    known_store();
}

/* ---------------- LAN discovery ---------------- */

typedef struct {
    struct netif *nif;
    ip4_addr_t ips[ARP_BATCH];
    int n;
    bool found[ARP_BATCH];
    uint8_t mac[ARP_BATCH][6];
} arp_batch_t;

static esp_err_t arp_send(void *ctx)
{
    auto *b = (arp_batch_t *)ctx;
    for (int i = 0; i < b->n; i++) etharp_request(b->nif, &b->ips[i]);
    return ESP_OK;
}

static esp_err_t arp_collect(void *ctx)
{
    auto *b = (arp_batch_t *)ctx;
    for (int i = 0; i < b->n; i++) {
        struct eth_addr *eth = nullptr;
        const ip4_addr_t *ip = nullptr;
        if (etharp_find_addr(b->nif, &b->ips[i], &eth, &ip) >= 0 && eth) {
            b->found[i] = true;
            memcpy(b->mac[i], eth->addr, 6);
        }
    }
    return ESP_OK;
}

static void lan_scan_task(void *)
{
    esp_netif_ip_info_t info;
    uint8_t self_mac[6];
    esp_netif_get_ip_info(s_netif, &info);
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);

    uint32_t self = ntohl(info.ip.addr);
    uint32_t gw = ntohl(info.gw.addr);
    uint32_t base = self & 0xFFFFFF00; /* sweep the local /24 even on wider subnets */

    {
        Lock l;
        s_host_count = 0;
        lan_host_t me = {};
        me.ip = info.ip.addr;
        memcpy(me.mac, self_mac, 6);
        me.is_self = true;
        s_hosts[s_host_count++] = me;
    }
    s_lan_gen = s_lan_gen + 1;

    arp_batch_t batch;
    batch.nif = (struct netif *)esp_netif_get_netif_impl(s_netif);
    for (uint32_t host = 1; host < 255 && s_state == WIFI_ST_CONNECTED;) {
        batch.n = 0;
        while (batch.n < ARP_BATCH && host < 255) {
            uint32_t addr = base | host++;
            if (addr == self) continue;
            batch.ips[batch.n].addr = htonl(addr);
            batch.found[batch.n] = false;
            batch.n++;
        }
        esp_netif_tcpip_exec(arp_send, &batch);
        vTaskDelay(pdMS_TO_TICKS(ARP_WAIT_MS));
        esp_netif_tcpip_exec(arp_collect, &batch);

        {
            Lock l;
            for (int i = 0; i < batch.n && s_host_count < LAN_MAX_HOSTS; i++) {
                if (!batch.found[i]) continue;
                lan_host_t h = {};
                h.ip = batch.ips[i].addr;
                memcpy(h.mac, batch.mac[i], 6);
                h.is_gateway = ntohl(h.ip) == gw;
                s_hosts[s_host_count++] = h;
            }
        }
        s_lan_progress = host * 100 / 255;
        s_lan_gen = s_lan_gen + 1;
    }

    {
        Lock l;
        std::sort(s_hosts, s_hosts + s_host_count,
                  [](const lan_host_t &a, const lan_host_t &b) { return ntohl(a.ip) < ntohl(b.ip); });
    }
    s_lan_progress = 100;
    s_lan_scanning = false;
    s_lan_gen = s_lan_gen + 1;
    ESP_LOGI(TAG, "LAN scan done: %d hosts", s_host_count);
    vTaskDelete(nullptr);
}

bool wifi_mgr_lan_scan_start(void)
{
    if (s_lan_scanning || s_state != WIFI_ST_CONNECTED) return false;
    s_lan_scanning = true;
    s_lan_progress = 0;
    if (xTaskCreatePinnedToCore(lan_scan_task, "lan_scan", 4096, nullptr, 3, nullptr, 0) != pdPASS) {
        s_lan_scanning = false;
        return false;
    }
    return true;
}

bool wifi_mgr_lan_scanning(int *progress_pct)
{
    if (progress_pct) *progress_pct = s_lan_progress;
    return s_lan_scanning;
}

uint32_t wifi_mgr_lan_gen(void)
{
    return s_lan_gen;
}

int wifi_mgr_lan_hosts(lan_host_t *out, int max)
{
    Lock l;
    int n = std::min(max, s_host_count);
    memcpy(out, s_hosts, n * sizeof(lan_host_t));
    return n;
}

int wifi_mgr_lan_host_count(void)
{
    return s_host_count;
}

/* ---------------- helpers ---------------- */

const char *wifi_auth_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    case WIFI_AUTH_OWE: return "OWE";
    case WIFI_AUTH_WPA3_ENT_192: return "WPA3-EAP192";
    case WIFI_AUTH_DPP: return "DPP";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-EAP";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/3-EAP";
    case WIFI_AUTH_WPA_ENTERPRISE: return "WPA-EAP";
    default: return "?";
    }
}

const char *wifi_cipher_str(wifi_cipher_type_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE: return "None";
    case WIFI_CIPHER_TYPE_WEP40: return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104: return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+CCMP";
    case WIFI_CIPHER_TYPE_AES_CMAC128: return "CMAC128";
    case WIFI_CIPHER_TYPE_SMS4: return "SMS4";
    case WIFI_CIPHER_TYPE_GCMP: return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256: return "GCMP256";
    default: return "?";
    }
}

int wifi_ap_bandwidth_mhz(const wifi_ap_record_t *ap)
{
    if (ap->second != WIFI_SECOND_CHAN_NONE) return 40;
    switch (ap->bandwidth) {
    case WIFI_BW40: return 40;
    case WIFI_BW80: return 80;
    case WIFI_BW160: return 160;
    default: return 20;
    }
}

float wifi_ap_center_channel(const wifi_ap_record_t *ap)
{
    if (ap->second == WIFI_SECOND_CHAN_ABOVE) return ap->primary + 2;
    if (ap->second == WIFI_SECOND_CHAN_BELOW) return ap->primary - 2;
    return ap->primary;
}

int wifi_rssi_quality(int rssi)
{
    if (rssi <= -100) return 0;
    if (rssi >= -50) return 100;
    return 2 * (rssi + 100);
}
