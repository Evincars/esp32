#pragma once

#include <cstddef>
#include <cstdint>

/* HTTP dashboard on port 80: a single page plus a read-only JSON API with the same
   data the device shows (system metrics, Wi-Fi, APs, LAN hosts, port scan, packets). */

bool web_server_start(void);
void web_server_stop(void);
bool web_server_running(void);
uint32_t web_server_requests(void);
/* IPv4 of the last client, "" if none yet. */
void web_server_last_client(char *buf, size_t len);
