#include "portscan.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "wifi_mgr.h"

static const char *TAG = "portscan";

#define BATCH 8             /* concurrent connects; keep below CONFIG_LWIP_MAX_SOCKETS minus the web server */
#define LAN_TIMEOUT_MS 400  /* LAN hosts answer a SYN within a few ms */
#define WAN_TIMEOUT_MS 1200
#define BANNER_TIMEOUT_MS 700

enum {
    GRAB_NONE,
    GRAB_GREETING, /* server talks first (SSH, FTP, SMTP...) */
    GRAB_HTTP,     /* send HEAD, report the Server header */
    GRAB_RTSP,
};

typedef struct {
    uint16_t port;
    const char *name;
    uint8_t grab;
} service_t;

static const service_t SERVICES[] = {
    {21, "ftp", GRAB_GREETING},        {22, "ssh", GRAB_GREETING},          {23, "telnet", GRAB_NONE},
    {25, "smtp", GRAB_GREETING},       {53, "domain", GRAB_NONE},           {80, "http", GRAB_HTTP},
    {81, "http-alt", GRAB_HTTP},       {88, "kerberos", GRAB_NONE},         {110, "pop3", GRAB_GREETING},
    {111, "rpcbind", GRAB_NONE},       {135, "msrpc", GRAB_NONE},           {139, "netbios-ssn", GRAB_NONE},
    {143, "imap", GRAB_GREETING},      {389, "ldap", GRAB_NONE},            {443, "https", GRAB_NONE},
    {445, "microsoft-ds", GRAB_NONE},  {465, "smtps", GRAB_NONE},           {515, "printer", GRAB_NONE},
    {548, "afp", GRAB_NONE},           {554, "rtsp", GRAB_RTSP},            {587, "submission", GRAB_GREETING},
    {631, "ipp", GRAB_HTTP},           {636, "ldaps", GRAB_NONE},           {873, "rsync", GRAB_GREETING},
    {993, "imaps", GRAB_NONE},         {995, "pop3s", GRAB_NONE},           {1080, "socks", GRAB_NONE},
    {1194, "openvpn", GRAB_NONE},      {1433, "ms-sql", GRAB_NONE},         {1521, "oracle", GRAB_NONE},
    {1723, "pptp", GRAB_NONE},         {1883, "mqtt", GRAB_NONE},           {2049, "nfs", GRAB_NONE},
    {2375, "docker", GRAB_HTTP},       {3000, "http-dev", GRAB_HTTP},       {3128, "squid", GRAB_HTTP},
    {3306, "mysql", GRAB_GREETING},    {3389, "rdp", GRAB_NONE},            {3689, "daap", GRAB_NONE},
    {4443, "https-alt", GRAB_NONE},    {5000, "upnp/http", GRAB_HTTP},      {5001, "https-alt", GRAB_NONE},
    {5060, "sip", GRAB_NONE},          {5357, "wsdapi", GRAB_HTTP},         {5432, "postgresql", GRAB_NONE},
    {5900, "vnc", GRAB_GREETING},      {6000, "x11", GRAB_NONE},            {6379, "redis", GRAB_NONE},
    {6443, "kube-api", GRAB_NONE},     {7000, "airplay", GRAB_HTTP},        {7547, "cwmp", GRAB_HTTP},
    {8000, "http-alt", GRAB_HTTP},     {8008, "http", GRAB_HTTP},           {8009, "ajp/cast", GRAB_NONE},
    {8080, "http-proxy", GRAB_HTTP},   {8081, "http-alt", GRAB_HTTP},       {8086, "influxdb", GRAB_HTTP},
    {8123, "home-assistant", GRAB_HTTP}, {8200, "dlna", GRAB_HTTP},         {8443, "https-alt", GRAB_NONE},
    {8883, "mqtts", GRAB_NONE},        {8888, "http-alt", GRAB_HTTP},       {9000, "http-alt", GRAB_HTTP},
    {9090, "web-console", GRAB_HTTP},  {9100, "jetdirect", GRAB_NONE},      {9200, "elasticsearch", GRAB_HTTP},
    {9443, "https-alt", GRAB_NONE},    {10000, "webmin", GRAB_HTTP},        {11211, "memcached", GRAB_NONE},
    {27017, "mongodb", GRAB_NONE},     {32400, "plex", GRAB_HTTP},          {49152, "upnp", GRAB_HTTP},
    {62078, "iphone-sync", GRAB_NONE},
};
#define SERVICE_COUNT (int)(sizeof(SERVICES) / sizeof(SERVICES[0]))

static SemaphoreHandle_t s_mutex;
static volatile bool s_running, s_stop;
static volatile uint32_t s_gen;
static ps_profile_t s_profile;
static ps_host_t s_hosts[PS_MAX_TARGETS];
static int s_host_count;
static ps_open_t *s_open; /* PS_MAX_OPEN entries in PSRAM */
static int s_open_count;
static volatile int s_hosts_done, s_ports_total, s_ports_done;
static volatile uint32_t s_current_ip;
static int64_t s_started_us, s_finished_us;

struct Lock {
    Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mutex); }
};

static const service_t *find_service(uint16_t port)
{
    for (int i = 0; i < SERVICE_COUNT; i++) {
        if (SERVICES[i].port == port) return &SERVICES[i];
    }
    return nullptr;
}

const char *portscan_service(uint16_t port)
{
    const service_t *s = find_service(port);
    return s ? s->name : "";
}

/* ---------------- banner grabbing ---------------- */

/* Copies the first line, replacing anything unprintable. */
static void first_line(const char *src, int n, char *out, size_t out_len)
{
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < out_len; i++) {
        char c = src[i];
        if (c == '\r' || c == '\n') {
            if (k) break;
            continue;
        }
        out[k++] = isprint((unsigned char)c) ? c : '.';
    }
    out[k] = 0;
}

static void http_banner(const char *buf, int n, char *out, size_t out_len)
{
    char status[48];
    first_line(buf, n, status, sizeof(status));
    const char *code = strchr(status, ' ');
    for (int i = 0; i + 7 < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && strncasecmp(buf + i, "server:", 7) == 0) {
            int j = i + 7;
            while (j < n && buf[j] == ' ') j++;
            char server[40];
            first_line(buf + j, n - j, server, sizeof(server));
            snprintf(out, out_len, "%s%s%.3s", server, code ? "  " : "", code ? code + 1 : "");
            return;
        }
    }
    strlcpy(out, status, out_len);
}

static void grab_banner(int fd, uint32_t ip, const service_t *svc, char *out, size_t out_len)
{
    out[0] = 0;
    if (!svc || svc->grab == GRAB_NONE) return;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval tv = {0, BANNER_TIMEOUT_MS * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[96];
    esp_ip4_addr_t a = {ip};
    if (svc->grab == GRAB_HTTP) {
        snprintf(req, sizeof(req), "HEAD / HTTP/1.0\r\nHost: " IPSTR "\r\n\r\n", IP2STR(&a));
        send(fd, req, strlen(req), 0);
    } else if (svc->grab == GRAB_RTSP) {
        snprintf(req, sizeof(req), "OPTIONS rtsp://" IPSTR "/ RTSP/1.0\r\nCSeq: 1\r\n\r\n", IP2STR(&a));
        send(fd, req, strlen(req), 0);
    }

    char buf[384];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return;

    if (svc->grab == GRAB_HTTP || svc->grab == GRAB_RTSP) {
        http_banner(buf, n, out, out_len);
    } else if (svc->port == 3306 && n > 6 && buf[4] == 0x0a) {
        /* MySQL greeting: 4-byte header, protocol 10, NUL-terminated version */
        snprintf(out, out_len, "MySQL %.*s", (int)strnlen(buf + 5, n - 5), buf + 5);
    } else {
        first_line(buf, n, out, out_len);
    }
}

/* ---------------- scanning ---------------- */

static void record_open(uint32_t ip, uint16_t port, uint16_t rtt, const char *banner)
{
    Lock l;
    if (s_open_count >= PS_MAX_OPEN) return;
    ps_open_t &o = s_open[s_open_count++];
    o.ip = ip;
    o.port = port;
    o.rtt_ms = rtt;
    strlcpy(o.banner, banner, sizeof(o.banner));
}

/* Scans up to BATCH ports of one host. Returns open/closed/filtered counts via the host. */
static void scan_batch(int host_idx, const uint16_t *ports, int n, int timeout_ms)
{
    uint32_t ip = s_hosts[host_idx].ip;
    int fds[BATCH];
    enum { PENDING, OPEN, CLOSED, FILTERED } state[BATCH];
    uint16_t rtt[BATCH] = {};
    int64_t t0 = esp_timer_get_time();

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;

    int pending = 0;
    for (int i = 0; i < n; i++) {
        state[i] = FILTERED;
        fds[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fds[i] < 0) continue;
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
        addr.sin_port = htons(ports[i]);
        int r = connect(fds[i], (struct sockaddr *)&addr, sizeof(addr));
        if (r == 0) {
            state[i] = OPEN;
        } else if (errno == EINPROGRESS) {
            state[i] = PENDING;
            pending++;
        } else {
            state[i] = errno == ECONNREFUSED ? CLOSED : FILTERED;
        }
    }

    while (pending > 0 && !s_stop) {
        int64_t left_us = (int64_t)timeout_ms * 1000 - (esp_timer_get_time() - t0);
        if (left_us <= 0) break;
        fd_set wset;
        FD_ZERO(&wset);
        int maxfd = -1;
        for (int i = 0; i < n; i++) {
            if (state[i] != PENDING) continue;
            FD_SET(fds[i], &wset);
            maxfd = std::max(maxfd, fds[i]);
        }
        struct timeval tv = {(time_t)(left_us / 1000000), (suseconds_t)(left_us % 1000000)};
        if (select(maxfd + 1, nullptr, &wset, nullptr, &tv) <= 0) break;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            if (state[i] != PENDING || !FD_ISSET(fds[i], &wset)) continue;
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                state[i] = OPEN;
                rtt[i] = (uint16_t)std::min<int64_t>((now - t0) / 1000, 65535);
            } else {
                state[i] = err == ECONNREFUSED ? CLOSED : FILTERED;
            }
            pending--;
        }
    }

    ps_host_t &h = s_hosts[host_idx];
    for (int i = 0; i < n; i++) {
        if (state[i] == OPEN) {
            char banner[48];
            grab_banner(fds[i], ip, find_service(ports[i]), banner, sizeof(banner));
            record_open(ip, ports[i], rtt[i], banner);
            h.open++;
        } else if (state[i] == CLOSED) {
            h.closed++;
        } else {
            h.filtered++;
        }
        if (fds[i] >= 0) close(fds[i]);
    }
    s_gen = s_gen + 1;
}

static void scan_task(void *)
{
    esp_netif_ip_info_t info = {};
    esp_netif_get_ip_info(wifi_mgr_netif(), &info);
    uint32_t mask = info.netmask.addr;

    uint16_t ports[BATCH];
    for (int h = 0; h < s_host_count && !s_stop; h++) {
        uint32_t ip = s_hosts[h].ip;
        s_current_ip = ip;
        bool local = mask && (ip & mask) == (info.ip.addr & mask);
        int timeout = local ? LAN_TIMEOUT_MS : WAN_TIMEOUT_MS;
        int total = s_profile == PS_PROFILE_TOP ? SERVICE_COUNT : 1024;
        s_ports_total = total;
        s_ports_done = 0;

        for (int k = 0; k < total && !s_stop;) {
            int n = 0;
            while (n < BATCH && k < total) {
                ports[n++] = s_profile == PS_PROFILE_TOP ? SERVICES[k].port : (uint16_t)(k + 1);
                k++;
            }
            scan_batch(h, ports, n, timeout);
            s_ports_done = k;
        }
        s_hosts[h].done = !s_stop;
        s_hosts_done = h + 1;
        s_gen = s_gen + 1;
    }

    {
        Lock l;
        std::sort(s_open, s_open + s_open_count, [](const ps_open_t &a, const ps_open_t &b) {
            if (a.ip != b.ip) return ntohl(a.ip) < ntohl(b.ip);
            return a.port < b.port;
        });
    }
    s_finished_us = esp_timer_get_time();
    s_running = false;
    s_gen = s_gen + 1;
    ESP_LOGI(TAG, "scan done: %d hosts, %d open ports, %d ms", s_hosts_done, s_open_count,
             (int)((s_finished_us - s_started_us) / 1000));
    vTaskDelete(nullptr);
}

bool portscan_start(const uint32_t *ips, int n, ps_profile_t profile)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_open) s_open = (ps_open_t *)heap_caps_calloc_prefer(PS_MAX_OPEN, sizeof(ps_open_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (s_running || n <= 0 || !s_open) return false;
    {
        Lock l;
        s_host_count = std::min(n, PS_MAX_TARGETS);
        for (int i = 0; i < s_host_count; i++) {
            s_hosts[i] = {};
            s_hosts[i].ip = ips[i];
        }
        s_open_count = 0;
    }
    s_profile = profile;
    s_hosts_done = 0;
    s_ports_done = 0;
    s_ports_total = 0;
    s_stop = false;
    s_running = true;
    s_started_us = esp_timer_get_time();
    s_finished_us = 0;
    s_gen = s_gen + 1;
    if (xTaskCreate(scan_task, "portscan", 6144, nullptr, 3, nullptr) != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

void portscan_stop(void)
{
    s_stop = true;
}

void portscan_status(ps_status_t *out)
{
    out->running = s_running;
    out->hosts_total = s_host_count;
    out->hosts_done = s_hosts_done;
    out->ports_total = s_ports_total;
    out->ports_done = s_ports_done;
    out->current_ip = s_current_ip;
    out->open_count = s_open_count;
    int64_t end = s_finished_us ? s_finished_us : esp_timer_get_time();
    out->elapsed_ms = s_started_us ? (uint32_t)((end - s_started_us) / 1000) : 0;
}

uint32_t portscan_gen(void)
{
    return s_gen;
}

int portscan_results(ps_open_t *out, int max)
{
    if (!s_mutex || !s_open) return 0;
    Lock l;
    int n = std::min(max, s_open_count);
    memcpy(out, s_open, n * sizeof(ps_open_t));
    return n;
}

int portscan_hosts(ps_host_t *out, int max)
{
    if (!s_mutex) return 0;
    Lock l;
    int n = std::min(max, s_host_count);
    memcpy(out, s_hosts, n * sizeof(ps_host_t));
    return n;
}
