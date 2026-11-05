#include "web_server.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "app_settings.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "oui.h"
#include "pkt_decode.h"
#include "portscan.h"
#include "repeater.h"
#include "sdkconfig.h"
#include "sniffer.h"
#include "sysmon.h"
#include "time_sync.h"
#include "ui/ui.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* Response scratch space, in PSRAM: httpd serves one request at a time. */
typedef struct {
    float c0[SYSMON_HISTORY], c1[SYSMON_HISTORY], tc[SYSMON_HISTORY];
    wifi_ap_record_t aps[WIFI_MAX_APS];
    lan_host_t hosts[LAN_MAX_HOSTS];
    ps_open_t open[PS_MAX_OPEN];
    ps_host_t ps_hosts[PS_MAX_TARGETS];
    sniff_pkt_t pkt;
    pkt_layer_t layers[PKT_MAX_LAYERS];
} scratch_t;

static scratch_t *W;
static httpd_handle_t s_server;
static volatile uint32_t s_requests;
static char s_last_client[16];

/* ---------------- JSON streaming ---------------- */

/* Buffers output and sends it as HTTP chunks, so responses of any size need
   no more than this small buffer. */
class Json {
public:
    explicit Json(httpd_req_t *req) : req_(req)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }

    void raw(const char *s) { put(s, strlen(s)); }

    void fmt(const char *f, ...) __attribute__((format(printf, 2, 3)))
    {
        char tmp[160];
        va_list ap;
        va_start(ap, f);
        int n = vsnprintf(tmp, sizeof(tmp), f, ap);
        va_end(ap);
        put(tmp, std::min<int>(n, sizeof(tmp) - 1));
    }

    void str(const char *s)
    {
        put("\"", 1);
        for (; *s; s++) {
            unsigned char c = *s;
            if (c == '"' || c == '\\') {
                char e[2] = {'\\', (char)c};
                put(e, 2);
            } else if (c < 0x20) {
                char e[8];
                snprintf(e, sizeof(e), "\\u%04x", c);
                put(e, 6);
            } else {
                put((const char *)&c, 1);
            }
        }
        put("\"", 1);
    }

    /* "key": */
    void key(const char *k)
    {
        str(k);
        put(":", 1);
    }

    void kv_str(const char *k, const char *v)
    {
        key(k);
        str(v);
    }

    esp_err_t finish()
    {
        flush();
        return httpd_resp_send_chunk(req_, nullptr, 0);
    }

private:
    void put(const char *s, size_t n)
    {
        while (n) {
            size_t take = std::min(n, sizeof(buf_) - len_);
            memcpy(buf_ + len_, s, take);
            len_ += take;
            s += take;
            n -= take;
            if (len_ == sizeof(buf_)) flush();
        }
    }

    void flush()
    {
        if (len_) httpd_resp_send_chunk(req_, buf_, len_);
        len_ = 0;
    }

    httpd_req_t *req_;
    char buf_[1024];
    size_t len_ = 0;
};

static void note_client(httpd_req_t *req)
{
    s_requests = s_requests + 1;
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) return;
    const uint8_t *a = nullptr;
    if (addr.ss_family == AF_INET) {
        a = (const uint8_t *)&((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    } else if (addr.ss_family == AF_INET6) {
        a = ((struct sockaddr_in6 *)&addr)->sin6_addr.s6_addr + 12; /* IPv4-mapped */
    }
    if (a) snprintf(s_last_client, sizeof(s_last_client), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

static void mac_str(char *buf, size_t len, const uint8_t *m)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ip_str(char *buf, size_t len, uint32_t ip)
{
    esp_ip4_addr_t a = {ip};
    snprintf(buf, len, IPSTR, IP2STR(&a));
}

/* ---------------- handlers ---------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    note_client(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static void json_floats(Json &j, const float *v, int n)
{
    j.raw("[");
    for (int i = 0; i < n; i++) j.fmt("%s%.1f", i ? "," : "", v[i]);
    j.raw("]");
}

static esp_err_t h_status(httpd_req_t *req)
{
    note_client(req);
    Json j(req);
    char buf[48];

    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    j.raw("{\"device\":{");
    j.kv_str("app", app->project_name);
    j.raw(",");
    j.kv_str("version", app->version);
    j.raw(",");
    j.kv_str("idf", esp_get_idf_version());
    j.fmt(",\"chip\":\"%s\",\"rev\":%d,\"cores\":%d,\"mhz\":%d},", CONFIG_IDF_TARGET, chip.revision, chip.cores,
          CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    const ui_palette_t *p = g_pal;
    j.fmt("\"theme\":{\"name\":\"%s\",\"bg\":\"#%06lx\",\"surface\":\"#%06lx\",\"border\":\"#%06lx\",", p->name,
          (unsigned long)p->bg, (unsigned long)p->surface, (unsigned long)p->border);
    j.fmt("\"text\":\"#%06lx\",\"dim\":\"#%06lx\",\"accent\":\"#%06lx\",\"good\":\"#%06lx\",\"bad\":\"#%06lx\"},",
          (unsigned long)p->text, (unsigned long)p->text_dim, (unsigned long)p->accent, (unsigned long)p->good,
          (unsigned long)p->bad);

    sysmon_t s;
    sysmon_get(&s);
    j.fmt("\"sys\":{\"cpu\":[%.1f,%.1f],\"temp\":%.1f,\"temp_min\":%.1f,\"temp_max\":%.1f,", s.cpu[0], s.cpu[1],
          s.temp_c, s.temp_min, s.temp_max);
    j.fmt("\"heap_free\":%lu,\"heap_total\":%lu,\"heap_min\":%lu,", (unsigned long)s.heap_free,
          (unsigned long)s.heap_total, (unsigned long)s.heap_min);
    j.fmt("\"psram_free\":%lu,\"psram_total\":%lu,", (unsigned long)s.psram_free, (unsigned long)s.psram_total);
    j.fmt("\"flash\":%lu,\"app_size\":%lu,\"app_part\":%lu,", (unsigned long)s.flash_size, (unsigned long)s.app_size,
          (unsigned long)s.app_part_size);
    j.fmt("\"batt_mv\":%d,\"batt_pct\":%d,\"usb\":%s,\"uptime\":%lu,\"tasks\":%d},", s.batt_mv, s.batt_pct,
          s.on_usb ? "true" : "false", (unsigned long)s.uptime_s, s.tasks);

    int n = sysmon_history(W->c0, W->c1, W->tc, SYSMON_HISTORY);
    j.raw("\"hist\":{\"cpu0\":");
    json_floats(j, W->c0, n);
    j.raw(",\"cpu1\":");
    json_floats(j, W->c1, n);
    j.raw(",\"temp\":");
    json_floats(j, W->tc, n);
    j.raw("},");

    wifi_conn_info_t w;
    bool up = wifi_mgr_conn_info(&w);
    static const char *STATES[] = {"idle", "connecting", "connected", "failed"};
    j.fmt("\"wifi\":{\"state\":\"%s\",", STATES[wifi_mgr_state()]);
    j.kv_str("text", wifi_mgr_state_text());
    mac_str(buf, sizeof(buf), w.mac);
    j.raw(",");
    j.kv_str("mac", buf);
    j.raw(",");
    j.kv_str("hostname", w.hostname);
    if (up) {
        j.raw(",");
        j.kv_str("ssid", w.ssid);
        mac_str(buf, sizeof(buf), w.bssid);
        j.raw(",");
        j.kv_str("bssid", buf);
        j.fmt(",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\"", w.rssi, w.channel, wifi_auth_str(w.authmode));
        ip_str(buf, sizeof(buf), w.ip.ip.addr);
        j.fmt(",\"ip\":\"%s\"", buf);
        ip_str(buf, sizeof(buf), w.ip.netmask.addr);
        j.fmt(",\"mask\":\"%s\"", buf);
        ip_str(buf, sizeof(buf), w.ip.gw.addr);
        j.fmt(",\"gw\":\"%s\"", buf);
        ip_str(buf, sizeof(buf), w.dns[0].addr);
        j.fmt(",\"dns\":\"%s\"", buf);
    }
    j.raw("},");

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    j.fmt("\"time\":{\"valid\":%s,\"local\":\"%s\",\"zone\":\"%s\"},", time_sync_valid() ? "true" : "false", buf,
          time_sync_zone_name(g_settings.timezone));
    repeater_status_t rep;
    repeater_status(&rep);
    j.fmt("\"repeater\":{\"running\":%s,", rep.running ? "true" : "false");
    j.kv_str("ssid", rep.ssid);
    j.fmt(",\"open\":%s,\"upstream\":%s,", rep.open ? "true" : "false", rep.upstream_up ? "true" : "false");
    j.kv_str("upstream_ssid", rep.upstream_ssid);
    ip_str(buf, sizeof(buf), rep.ap_ip);
    j.fmt(",\"gw\":\"%s\",\"clients\":%d,\"wan_rx\":%llu,\"wan_tx\":%llu,\"lan_rx\":%llu,\"lan_tx\":%llu,\"peers\":[",
          buf, rep.clients, (unsigned long long)rep.wan_rx, (unsigned long long)rep.wan_tx,
          (unsigned long long)rep.lan_rx, (unsigned long long)rep.lan_tx);
    repeater_client_t peers[REP_MAX_CLIENTS];
    int np = repeater_clients(peers, REP_MAX_CLIENTS);
    for (int i = 0; i < np; i++) {
        ip_str(buf, sizeof(buf), peers[i].ip);
        j.fmt("%s{\"ip\":\"%s\"", i ? "," : "", buf);
        mac_str(buf, sizeof(buf), peers[i].mac);
        j.fmt(",\"mac\":\"%s\"", buf);
        const char *v = oui_vendor(peers[i].mac);
        j.raw(",");
        j.kv_str("vendor", v ? v : "");
        j.fmt(",\"rssi\":%d}", peers[i].rssi);
    }
    j.raw("]},");

    j.fmt("\"web\":{\"requests\":%lu}}", (unsigned long)s_requests);
    return j.finish();
}

static esp_err_t h_aps(httpd_req_t *req)
{
    note_client(req);
    wifi_ap_record_t *aps = W->aps;
    int n = wifi_mgr_get_aps(aps, WIFI_MAX_APS);
    Json j(req);
    char buf[20];
    j.fmt("{\"scanning\":%s,\"aps\":[", wifi_mgr_scanning() ? "true" : "false");
    for (int i = 0; i < n; i++) {
        const wifi_ap_record_t &a = aps[i];
        j.raw(i ? ",{" : "{");
        j.kv_str("ssid", (const char *)a.ssid);
        mac_str(buf, sizeof(buf), a.bssid);
        j.raw(",");
        j.kv_str("bssid", buf);
        const char *v = oui_vendor(a.bssid);
        j.raw(",");
        j.kv_str("vendor", v ? v : "");
        j.fmt(",\"ch\":%d,\"bw\":%d,\"rssi\":%d,\"auth\":\"%s\",\"wps\":%s}", a.primary, wifi_ap_bandwidth_mhz(&a),
              a.rssi, wifi_auth_str(a.authmode), a.wps ? "true" : "false");
    }
    j.raw("]}");
    return j.finish();
}

static esp_err_t h_hosts(httpd_req_t *req)
{
    note_client(req);
    lan_host_t *hosts = W->hosts;
    int n = wifi_mgr_lan_hosts(hosts, LAN_MAX_HOSTS);
    int pct = 0;
    bool busy = wifi_mgr_lan_scanning(&pct);
    Json j(req);
    char buf[20];
    j.fmt("{\"scanning\":%s,\"progress\":%d,\"hosts\":[", busy ? "true" : "false", pct);
    for (int i = 0; i < n; i++) {
        ip_str(buf, sizeof(buf), hosts[i].ip);
        j.fmt("%s{\"ip\":\"%s\"", i ? "," : "", buf);
        mac_str(buf, sizeof(buf), hosts[i].mac);
        j.fmt(",\"mac\":\"%s\"", buf);
        const char *v = oui_vendor(hosts[i].mac);
        j.raw(",");
        j.kv_str("vendor", v ? v : "");
        j.fmt(",\"self\":%s,\"gw\":%s}", hosts[i].is_self ? "true" : "false", hosts[i].is_gateway ? "true" : "false");
    }
    j.raw("]}");
    return j.finish();
}

static esp_err_t h_ports(httpd_req_t *req)
{
    note_client(req);
    ps_open_t *open = W->open;
    ps_host_t *hosts = W->ps_hosts;
    ps_status_t st;
    portscan_status(&st);
    int no = portscan_results(open, PS_MAX_OPEN);
    int nh = portscan_hosts(hosts, PS_MAX_TARGETS);
    Json j(req);
    char buf[20];
    j.fmt("{\"running\":%s,\"hosts_done\":%d,\"hosts_total\":%d,\"ports_done\":%d,\"ports_total\":%d,\"elapsed_ms\":%lu,",
          st.running ? "true" : "false", st.hosts_done, st.hosts_total, st.ports_done, st.ports_total,
          (unsigned long)st.elapsed_ms);
    j.raw("\"hosts\":[");
    for (int i = 0; i < nh; i++) {
        ip_str(buf, sizeof(buf), hosts[i].ip);
        j.fmt("%s{\"ip\":\"%s\",\"open\":%d,\"closed\":%d,\"filtered\":%d,\"done\":%s}", i ? "," : "", buf,
              hosts[i].open, hosts[i].closed, hosts[i].filtered, hosts[i].done ? "true" : "false");
    }
    j.raw("],\"open\":[");
    for (int i = 0; i < no; i++) {
        ip_str(buf, sizeof(buf), open[i].ip);
        j.fmt("%s{\"ip\":\"%s\",\"port\":%u,\"service\":\"%s\",\"rtt\":%u,", i ? "," : "", buf, open[i].port,
              portscan_service(open[i].port), open[i].rtt_ms);
        j.kv_str("banner", open[i].banner);
        j.raw("}");
    }
    j.raw("]}");
    return j.finish();
}

static uint32_t query_u32(httpd_req_t *req, const char *key, uint32_t def)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return def;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return def;
    return strtoul(v, nullptr, 10);
}

static esp_err_t h_packets(httpd_req_t *req)
{
    note_client(req);
    uint32_t since = query_u32(req, "since", 0);
    sniff_pkt_t &pkt = W->pkt;
    pkt_summary_t sum;
    sniff_stats_t st;
    sniffer_stats(&st);

    Json j(req);
    j.fmt("{\"running\":%s,\"mode\":\"%s\",\"filter\":\"%s\",\"channel\":%d,\"last\":%lu,",
          sniffer_running() ? "true" : "false", sniffer_mode() == SNIFF_MODE_LAN ? "lan" : "air",
          sniffer_filter_name(sniffer_filter()), sniffer_channel(), (unsigned long)sniffer_last_seq());
    j.fmt("\"stats\":{\"total\":%lu,\"stored\":%lu,\"bytes\":%llu,\"arp\":%lu,\"icmp\":%lu,\"tcp\":%lu,\"udp\":%lu,",
          (unsigned long)st.total, (unsigned long)st.stored, (unsigned long long)st.bytes, (unsigned long)st.arp,
          (unsigned long)st.icmp, (unsigned long)st.tcp, (unsigned long)st.udp);
    j.fmt("\"dns\":%lu,\"dhcp\":%lu,\"web\":%lu,\"mgmt\":%lu,\"data\":%lu,\"eapol\":%lu,\"other\":%lu},",
          (unsigned long)st.dns, (unsigned long)st.dhcp, (unsigned long)st.web, (unsigned long)st.mgmt,
          (unsigned long)st.data, (unsigned long)st.eapol, (unsigned long)st.other);

    /* oldest first, at most 60 frames newer than `since` */
    uint32_t last = sniffer_last_seq();
    uint32_t first = std::max<uint32_t>(since + 1, last > 59 ? last - 59 : 1);
    j.raw("\"packets\":[");
    bool comma = false;
    for (uint32_t seq = first; seq <= last && seq != 0; seq++) {
        if (!sniffer_get(seq, &pkt)) continue;
        pkt_summarize(&pkt, &sum);
        j.fmt("%s{\"seq\":%lu,\"t\":%lu,\"len\":%u,\"dir\":\"%s\",\"osi\":%d,", comma ? "," : "",
              (unsigned long)seq, (unsigned long)pkt.t_ms, pkt.len,
              pkt.src == SNIFF_SRC_TX ? "tx" : pkt.src == SNIFF_SRC_RX ? "rx" : "air", sum.top_osi);
        j.kv_str("proto", sum.proto);
        j.raw(",");
        j.kv_str("src", sum.src);
        j.raw(",");
        j.kv_str("dst", sum.dst);
        j.raw(",");
        j.kv_str("info", sum.info);
        j.raw("}");
        comma = true;
    }
    j.raw("]}");
    return j.finish();
}

static esp_err_t h_packet(httpd_req_t *req)
{
    note_client(req);
    sniff_pkt_t &pkt = W->pkt;
    pkt_layer_t *layers = W->layers;
    uint32_t seq = query_u32(req, "seq", 0);
    if (!sniffer_get(seq, &pkt)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"packet no longer in the buffer\"}");
    }
    int n = pkt_dissect(&pkt, layers, PKT_MAX_LAYERS);
    Json j(req);
    j.fmt("{\"seq\":%lu,\"len\":%u,\"caplen\":%u,\"layers\":[", (unsigned long)seq, pkt.len, pkt.caplen);
    for (int i = 0; i < n; i++) {
        const pkt_layer_t &l = layers[i];
        j.fmt("%s{\"osi\":%d,\"osi_name\":\"%s\",\"off\":%u,\"len\":%u,", i ? "," : "", l.osi, pkt_osi_name(l.osi),
              l.off, l.len);
        j.kv_str("name", l.name);
        j.raw(",\"fields\":[");
        for (int f = 0; f < l.nfields; f++) {
            j.raw(f ? ",[" : "[");
            j.str(l.f[f].k);
            j.raw(",");
            j.str(l.f[f].v);
            j.raw("]");
        }
        j.raw("]}");
    }
    j.raw("],\"hex\":\"");
    for (int i = 0; i < pkt.caplen; i++) j.fmt("%02x", pkt.data[i]);
    j.raw("\"}");
    return j.finish();
}

static esp_err_t h_scan(httpd_req_t *req)
{
    note_client(req);
    bool ok = wifi_mgr_scan_start();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* ---------------- lifecycle ---------------- */

bool web_server_start(void)
{
    if (s_server) return true;
    if (!W) W = (scratch_t *)heap_caps_calloc_prefer(1, sizeof(scratch_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (!W) return false;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.core_id = 0;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = nullptr;
        return false;
    }
    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, h_index, nullptr},
        {"/api/status", HTTP_GET, h_status, nullptr},
        {"/api/aps", HTTP_GET, h_aps, nullptr},
        {"/api/hosts", HTTP_GET, h_hosts, nullptr},
        {"/api/ports", HTTP_GET, h_ports, nullptr},
        {"/api/packets", HTTP_GET, h_packets, nullptr},
        {"/api/packet", HTTP_GET, h_packet, nullptr},
        {"/api/scan", HTTP_POST, h_scan, nullptr},
    };
    for (const httpd_uri_t &u : uris) httpd_register_uri_handler(s_server, &u);
    ESP_LOGI(TAG, "dashboard listening on port %d", cfg.server_port);
    return true;
}

void web_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = nullptr;
}

bool web_server_running(void)
{
    return s_server != nullptr;
}

uint32_t web_server_requests(void)
{
    return s_requests;
}

void web_server_last_client(char *buf, size_t len)
{
    strlcpy(buf, s_last_client, len);
}
