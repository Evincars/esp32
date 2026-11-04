#pragma once

#include <cstdint>

/* nmap-style TCP connect scan: a batch of non-blocking connects per round, open
   ports get a short banner grab (greeting line, HTTP Server header, RTSP...). */

#define PS_MAX_TARGETS 64
#define PS_MAX_OPEN 160

typedef enum {
    PS_PROFILE_TOP,  /* ~75 well-known service ports */
    PS_PROFILE_1024, /* every port 1..1024 */
} ps_profile_t;

typedef struct {
    uint32_t ip; /* network byte order */
    uint16_t port;
    uint16_t rtt_ms;
    char banner[48];
} ps_open_t;

typedef struct {
    uint32_t ip;
    uint16_t open, closed, filtered;
    bool done;
} ps_host_t;

typedef struct {
    bool running;
    int hosts_total, hosts_done;
    int ports_total, ports_done; /* for the host being scanned */
    uint32_t current_ip;
    int open_count;
    uint32_t elapsed_ms;
} ps_status_t;

/* ips in network byte order. Returns false if a scan is already running. */
bool portscan_start(const uint32_t *ips, int n, ps_profile_t profile);
void portscan_stop(void);
void portscan_status(ps_status_t *out);
uint32_t portscan_gen(void); /* bumps whenever results change */
int portscan_results(ps_open_t *out, int max);
int portscan_hosts(ps_host_t *out, int max);
/* Service name for a TCP port, or "" if not in the table. */
const char *portscan_service(uint16_t port);
