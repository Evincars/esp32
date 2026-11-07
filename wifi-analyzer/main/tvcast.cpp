#include "tvcast.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "tvcast";

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900

/* ---------------- tiny blocking HTTP/1.1 client ---------------- */

/* Opens a TCP connection to ip:port, sends one request and reads the whole
   response into `resp` (NUL-terminated, truncated to cap). Returns the HTTP
   status code, or a negative lwIP error. `extra_hdrs` and `body` may be null. */
static int http_do(uint32_t ip, uint16_t port, const char *method, const char *path, const char *extra_hdrs,
                   const char *body, char *resp, size_t cap)
{
    if (resp && cap) resp[0] = 0;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -2;
    }

    char esp_ip[16];
    esp_ip4_addr_t ipa = {ip};
    snprintf(esp_ip, sizeof(esp_ip), IPSTR, IP2STR(&ipa));

    int blen = body ? (int)strlen(body) : 0;
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHOST: %s:%u\r\nConnection: close\r\n"
                     "Content-Length: %d\r\n%s%s\r\n",
                     method, path, esp_ip, port, blen,
                     blen ? "Content-Type: application/x-www-form-urlencoded\r\n" : "", extra_hdrs ? extra_hdrs : "");
    if (n < 0 || n >= (int)sizeof(req)) {
        close(fd);
        return -3;
    }
    if (send(fd, req, n, 0) < 0 || (blen && send(fd, body, blen, 0) < 0)) {
        close(fd);
        return -4;
    }

    size_t got = 0;
    if (resp && cap) {
        while (got + 1 < cap) {
            int r = recv(fd, resp + got, cap - 1 - got, 0);
            if (r <= 0) break;
            got += r;
        }
        resp[got] = 0;
    }
    close(fd);

    if (got < 12 || strncmp(resp, "HTTP/1.", 7) != 0) return -5;
    return atoi(resp + 9); /* "HTTP/1.1 201 ..." -> 201 */
}

/* Case-insensitive lookup of a header value into `out` (one line, trimmed). */
static bool header_val(const char *resp, const char *name, char *out, size_t cap)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = resp; *p;) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (p + linelen) - v;
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return true;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return false;
}

/* Splits an "http://host:port/path" URL into parts. */
static bool parse_url(const char *url, char *host, size_t hcap, uint16_t *port, char *path, size_t pcap)
{
    if (strncasecmp(url, "http://", 7) != 0) return false;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    const char *colon = strchr(h, ':');
    const char *hostend = slash;
    *port = 80;
    if (colon && (!slash || colon < slash)) {
        *port = (uint16_t)atoi(colon + 1);
        hostend = colon;
    }
    if (!hostend) hostend = h + strlen(h);
    size_t hl = hostend - h;
    if (hl >= hcap) return false;
    memcpy(host, h, hl);
    host[hl] = 0;
    snprintf(path, pcap, "%s", slash ? slash : "/");
    return true;
}

static uint32_t resolve(const char *host)
{
    uint32_t ip = inet_addr(host);
    if (ip != INADDR_NONE) return ip;
    struct hostent *he = gethostbyname(host);
    if (he && he->h_addr_list[0]) return *(uint32_t *)he->h_addr_list[0];
    return 0;
}

/* ---------------- SSDP discovery ---------------- */

int tvcast_discover(tv_device_t *out, int max, int timeout_ms)
{
    int found = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return 0;

    struct timeval tv = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SSDP_PORT);
    dst.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    const char *msearch = "M-SEARCH * HTTP/1.1\r\n"
                          "HOST: 239.255.255.250:1900\r\n"
                          "MAN: \"ssdp:discover\"\r\n"
                          "MX: 2\r\n"
                          "ST: urn:dial-multiscreen-org:service:dial:1\r\n\r\n";
    /* a couple of probes, in case one is lost */
    for (int i = 0; i < 2; i++) sendto(fd, msearch, strlen(msearch), 0, (struct sockaddr *)&dst, sizeof(dst));

    int64_t deadline_ms = esp_log_timestamp() + timeout_ms;
    char buf[768];
    while (found < max && (int64_t)esp_log_timestamp() < deadline_ms) {
        struct sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        int r = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        buf[r] = 0;

        char loc[128];
        if (!header_val(buf, "LOCATION", loc, sizeof(loc))) continue;

        uint32_t sip = from.sin_addr.s_addr;
        bool dup = false;
        for (int i = 0; i < found; i++) dup |= out[i].ip == sip;
        if (dup) continue;

        /* GET the device description; its Application-URL header is the DIAL base */
        char host[64], path[96];
        uint16_t port;
        if (!parse_url(loc, host, sizeof(host), &port, path, sizeof(path))) continue;
        uint32_t dip = resolve(host);
        if (!dip) dip = sip;

        tv_device_t *d = &out[found];
        memset(d, 0, sizeof(*d));
        d->ip = dip;

        char *desc = (char *)malloc(2048);
        if (desc) {
            int st = http_do(dip, port, "GET", path, nullptr, nullptr, desc, 2048);
            char appurl[96];
            if (st > 0 && header_val(desc, "Application-URL", appurl, sizeof(appurl))) {
                snprintf(d->app_url, sizeof(d->app_url), "%s", appurl);
            }
            /* friendlyName from the XML body */
            char *fn = strstr(desc, "<friendlyName>");
            if (fn) {
                fn += 14;
                char *end = strstr(fn, "</friendlyName>");
                if (end) {
                    size_t l = end - fn;
                    if (l >= sizeof(d->name)) l = sizeof(d->name) - 1;
                    memcpy(d->name, fn, l);
                    d->name[l] = 0;
                }
            }
            free(desc);
        }
        /* DIAL base defaults: many TVs expose it on :8008/apps */
        char ahost[64], apath[96];
        uint16_t aport = 8008;
        if (d->app_url[0] && parse_url(d->app_url, ahost, sizeof(ahost), &aport, apath, sizeof(apath))) {
            d->port = aport;
        } else {
            d->port = 8008;
            esp_ip4_addr_t ipa = {dip};
            snprintf(d->app_url, sizeof(d->app_url), "http://" IPSTR ":8008/apps", IP2STR(&ipa));
        }
        if (!d->name[0]) {
            esp_ip4_addr_t ipa = {dip};
            snprintf(d->name, sizeof(d->name), IPSTR, IP2STR(&ipa));
        }
        found++;
    }
    close(fd);
    ESP_LOGI(TAG, "DIAL discovery: %d device(s)", found);
    return found;
}

/* ---------------- app control ---------------- */

/* Builds "<app_url path>/<app>" and issues the request. */
static int app_request(const tv_device_t *d, const char *method, const char *app, const char *body, char *resp,
                       size_t cap)
{
    char host[64], base[96];
    uint16_t port;
    if (!parse_url(d->app_url, host, sizeof(host), &port, base, sizeof(base))) return -10;
    uint32_t ip = d->ip ? d->ip : resolve(host);
    if (!ip) return -11;

    size_t bl = strlen(base);
    char path[160];
    if (bl && base[bl - 1] == '/') base[bl - 1] = 0; /* avoid a double slash */
    snprintf(path, sizeof(path), "%s/%s", base, app);
    return http_do(ip, port, method, path, nullptr, body, resp, cap);
}

int tvcast_launch(const tv_device_t *d, const char *app, const char *body)
{
    char resp[256];
    return app_request(d, "POST", app, body, resp, sizeof(resp));
}

int tvcast_stop(const tv_device_t *d, const char *app)
{
    /* DELETE the running instance resource; most DIAL servers accept the app URL */
    char resp[256];
    char run[64];
    snprintf(run, sizeof(run), "%s/run", app);
    int st = app_request(d, "DELETE", run, nullptr, resp, sizeof(resp));
    if (st > 0) return st;
    return app_request(d, "DELETE", app, nullptr, resp, sizeof(resp));
}

int tvcast_app_state(const tv_device_t *d, const char *app, char *state, size_t nstate)
{
    if (state && nstate) state[0] = 0;
    char resp[1024];
    int st = app_request(d, "GET", app, nullptr, resp, sizeof(resp));
    if (st <= 0) return st;
    /* the DIAL app XML carries <state>running</state> (or stopped/hidden) */
    char *s = strstr(resp, "<state>");
    if (s && state && nstate) {
        s += 7;
        char *e = strstr(s, "</state>");
        if (e) {
            size_t l = e - s;
            if (l >= nstate) l = nstate - 1;
            memcpy(state, s, l);
            state[l] = 0;
        }
    }
    return st;
}

int tvcast_youtube(const tv_device_t *d, const char *video_id)
{
    char body[80];
    body[0] = 0;
    if (video_id && video_id[0]) snprintf(body, sizeof(body), "v=%s", video_id);
    return tvcast_launch(d, "YouTube", body[0] ? body : nullptr);
}
