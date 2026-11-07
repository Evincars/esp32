#include "cli.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "oui.h"
#include "portscan.h"
#include "sysmon.h"
#include "tvcast.h"
#include "wifi_mgr.h"

/* ---------------- output helpers ---------------- */

static void pf(cli_out_fn out, void *ctx, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void pf(cli_out_fn out, void *ctx, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out(buf, ctx);
}

static void ip_str(uint32_t ip, char *buf, size_t n)
{
    esp_ip4_addr_t a = {ip};
    snprintf(buf, n, IPSTR, IP2STR(&a));
}

/* ---------------- discovered-TV cache (shared across tv commands) ---------------- */

static tv_device_t s_tv[TV_MAX_DEVICES];
static int s_tv_count;

/* ---------------- commands ---------------- */

static void cmd_help(cli_out_fn out, void *ctx)
{
    out("Commands:\r\n", ctx);
    out("  help                 this list\r\n", ctx);
    out("  sys                  CPU load, frequency, RAM, temperature, uptime\r\n", ctx);
    out("  net                  Wi-Fi connection (SSID, IP, RSSI, gateway)\r\n", ctx);
    out("  scan                 scan for Wi-Fi access points\r\n", ctx);
    out("  hosts                discover LAN hosts (ARP sweep of the /24)\r\n", ctx);
    out("  ports <ip> [full]    TCP port scan of one host (top ports, or 1-1024)\r\n", ctx);
    out("  tv                   discover DIAL cast devices (Samsung TV etc.)\r\n", ctx);
    out("  tv <n> youtube [id]  open YouTube on device n, optionally play a video id\r\n", ctx);
    out("  tv <n> app <Name>    launch an app by DIAL name (e.g. Netflix)\r\n", ctx);
    out("  tv <n> stop <Name>   stop an app\r\n", ctx);
    out("  reboot               restart the board\r\n", ctx);
    out("  exit                 close the session\r\n", ctx);
}

static void cmd_sys(cli_out_fn out, void *ctx)
{
    sysmon_t s;
    sysmon_get(&s);
    pf(out, ctx, "CPU   core0 %d%%  core1 %d%%  @ %lu MHz\r\n", (int)s.cpu[0], (int)s.cpu[1],
       (unsigned long)s.cpu_freq_mhz);
    pf(out, ctx, "RAM   internal free %lu KB / %lu KB   PSRAM free %lu KB / %lu KB\r\n",
       (unsigned long)(s.heap_free >> 10), (unsigned long)(s.heap_total >> 10), (unsigned long)(s.psram_free >> 10),
       (unsigned long)(s.psram_total >> 10));
    if (s.temp_ok) pf(out, ctx, "Temp  %d.%d C\r\n", (int)s.temp_c, (int)(s.temp_c * 10) % 10);
    pf(out, ctx, "Up    %lu s   tasks %d\r\n", (unsigned long)s.uptime_s, s.tasks);
}

static void cmd_net(cli_out_fn out, void *ctx)
{
    wifi_conn_info_t info;
    if (!wifi_mgr_conn_info(&info)) {
        out("Not connected to Wi-Fi.\r\n", ctx);
        return;
    }
    char ip[16], gw[16];
    ip_str(info.ip.ip.addr, ip, sizeof(ip));
    ip_str(info.ip.gw.addr, gw, sizeof(gw));
    pf(out, ctx, "SSID  %s   RSSI %d dBm   ch %d\r\n", info.ssid, info.rssi, info.channel);
    pf(out, ctx, "IP    %s   GW %s   host %s\r\n", ip, gw, info.hostname);
}

static void cmd_scan(cli_out_fn out, void *ctx)
{
    out("Scanning...\r\n", ctx);
    uint32_t gen = wifi_mgr_scan_gen();
    wifi_mgr_scan_start();
    for (int i = 0; i < 80 && (wifi_mgr_scanning() || wifi_mgr_scan_gen() == gen); i++)
        vTaskDelay(pdMS_TO_TICKS(100));

    static wifi_ap_record_t aps[32];
    int n = wifi_mgr_get_aps(aps, 32);
    pf(out, ctx, "%d access points:\r\n", n);
    for (int i = 0; i < n; i++) {
        pf(out, ctx, "  %-28.28s %02x:%02x:%02x:%02x:%02x:%02x  ch%-3d %4d dBm  %s\r\n",
           aps[i].ssid[0] ? (char *)aps[i].ssid : "(hidden)", aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
           aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5], aps[i].primary, aps[i].rssi,
           wifi_auth_str(aps[i].authmode));
    }
}

static void cmd_hosts(cli_out_fn out, void *ctx)
{
    if (wifi_mgr_state() != WIFI_ST_CONNECTED) {
        out("Not connected to Wi-Fi.\r\n", ctx);
        return;
    }
    out("Discovering LAN hosts", ctx);
    wifi_mgr_lan_scan_start();
    int pct = 0;
    for (int i = 0; i < 120 && wifi_mgr_lan_scanning(&pct); i++) {
        if (i % 5 == 0) out(".", ctx);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    out("\r\n", ctx);

    static lan_host_t hosts[LAN_MAX_HOSTS];
    int n = wifi_mgr_lan_hosts(hosts, LAN_MAX_HOSTS);
    pf(out, ctx, "%d host(s):\r\n", n);
    for (int i = 0; i < n; i++) {
        char ip[16];
        ip_str(hosts[i].ip, ip, sizeof(ip));
        const char *vendor = oui_vendor(hosts[i].mac);
        pf(out, ctx, "  %-15s %02x:%02x:%02x:%02x:%02x:%02x %s%s\r\n", ip, hosts[i].mac[0], hosts[i].mac[1],
           hosts[i].mac[2], hosts[i].mac[3], hosts[i].mac[4], hosts[i].mac[5],
           hosts[i].is_self ? "(this board) " : hosts[i].is_gateway ? "(gateway) " : "", vendor ? vendor : "");
    }
}

static void cmd_ports(cli_out_fn out, void *ctx, const char *ipstr, bool full)
{
    ip4_addr_t a;
    if (!ip4addr_aton(ipstr, &a) || a.addr == 0) {
        out("usage: ports <ip> [full]\r\n", ctx);
        return;
    }
    uint32_t ip = a.addr;
    if (!portscan_start(&ip, 1, full ? PS_PROFILE_1024 : PS_PROFILE_TOP)) {
        out("Could not start scan (busy or offline).\r\n", ctx);
        return;
    }
    pf(out, ctx, "Scanning %s (%s)...\r\n", ipstr, full ? "1-1024" : "top ports");
    ps_status_t st;
    for (int i = 0; i < 600; i++) {
        portscan_status(&st);
        if (!st.running) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    static ps_open_t open[PS_MAX_OPEN];
    int no = portscan_results(open, PS_MAX_OPEN);
    int shown = 0;
    for (int i = 0; i < no; i++) {
        if (open[i].ip != ip) continue;
        const char *svc = portscan_service(open[i].port);
        pf(out, ctx, "  %5u/tcp  open  %-14s %s\r\n", open[i].port, svc[0] ? svc : "?", open[i].banner);
        shown++;
    }
    pf(out, ctx, "%d open port(s).\r\n", shown);
}

static void cmd_tv_discover(cli_out_fn out, void *ctx)
{
    out("Searching for DIAL devices (SSDP)...\r\n", ctx);
    s_tv_count = tvcast_discover(s_tv, TV_MAX_DEVICES, 3000);
    if (!s_tv_count) {
        out("No DIAL devices found. (Is the TV on and on this network?)\r\n", ctx);
        return;
    }
    for (int i = 0; i < s_tv_count; i++) {
        char ip[16];
        ip_str(s_tv[i].ip, ip, sizeof(ip));
        pf(out, ctx, "  [%d] %-24.24s %s:%u\r\n", i, s_tv[i].name, ip, s_tv[i].port);
    }
    out("Use:  tv <n> youtube [videoId]   |   tv <n> app <Name>\r\n", ctx);
}

static void cmd_tv(cli_out_fn out, void *ctx, int argc, char **argv)
{
    if (argc < 2) {
        cmd_tv_discover(out, ctx);
        return;
    }
    int n = atoi(argv[1]);
    if (n < 0 || n >= s_tv_count) {
        out("No such device. Run 'tv' to list them first.\r\n", ctx);
        return;
    }
    const tv_device_t *d = &s_tv[n];
    const char *verb = argc >= 3 ? argv[2] : "status";

    int st;
    if (strcmp(verb, "youtube") == 0) {
        st = tvcast_youtube(d, argc >= 4 ? argv[3] : "");
        pf(out, ctx, "YouTube launch -> HTTP %d%s\r\n", st, argc >= 4 ? " (playing video)" : "");
    } else if (strcmp(verb, "app") == 0 && argc >= 4) {
        st = tvcast_launch(d, argv[3], nullptr);
        pf(out, ctx, "launch %s -> HTTP %d\r\n", argv[3], st);
    } else if (strcmp(verb, "stop") == 0 && argc >= 4) {
        st = tvcast_stop(d, argv[3]);
        pf(out, ctx, "stop %s -> HTTP %d\r\n", argv[3], st);
    } else {
        char state[24];
        st = tvcast_app_state(d, "YouTube", state, sizeof(state));
        pf(out, ctx, "%s: YouTube is %s (HTTP %d)\r\n", d->name, state[0] ? state : "unknown", st);
    }
}

/* ---------------- dispatch ---------------- */

void cli_execute(const char *line, cli_out_fn out, void *ctx)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", line);

    char *argv[8];
    int argc = 0;
    char *p = buf;
    while (argc < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) return;

    const char *cmd = argv[0];
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) cmd_help(out, ctx);
    else if (strcmp(cmd, "sys") == 0 || strcmp(cmd, "status") == 0) cmd_sys(out, ctx);
    else if (strcmp(cmd, "net") == 0) cmd_net(out, ctx);
    else if (strcmp(cmd, "scan") == 0) cmd_scan(out, ctx);
    else if (strcmp(cmd, "hosts") == 0 || strcmp(cmd, "lan") == 0) cmd_hosts(out, ctx);
    else if (strcmp(cmd, "ports") == 0) {
        if (argc < 2) out("usage: ports <ip> [full]\r\n", ctx);
        else cmd_ports(out, ctx, argv[1], argc >= 3 && strcmp(argv[2], "full") == 0);
    } else if (strcmp(cmd, "tv") == 0) cmd_tv(out, ctx, argc, argv);
    else if (strcmp(cmd, "reboot") == 0) {
        out("Rebooting...\r\n", ctx);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        pf(out, ctx, "unknown command '%s' (try 'help')\r\n", cmd);
    }
}

const char *cli_banner(void)
{
    return "\r\n  WiFi Toolkit - ESP32-S3 remote console\r\n  type 'help' for commands\r\n\r\n";
}

const char *cli_prompt(void) { return "esp32> "; }
