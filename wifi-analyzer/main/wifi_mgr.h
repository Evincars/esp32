#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_netif.h"
#include "esp_wifi.h"

/* Owns the Wi-Fi driver: scanning, connecting, saved networks and LAN discovery.
   All getters copy data out under a mutex, so they are safe from the LVGL task. */

#define WIFI_MAX_APS 48
#define WIFI_MAX_KNOWN 8
#define LAN_MAX_HOSTS 64

typedef enum {
    WIFI_ST_IDLE,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_FAILED,
} wifi_state_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    wifi_second_chan_t second;
    wifi_auth_mode_t authmode;
    bool phy_11b, phy_11g, phy_11n, phy_11ax;
    esp_netif_ip_info_t ip;
    esp_ip4_addr_t dns[2];
    bool has_ip6;
    esp_ip6_addr_t ip6;
    char hostname[32];
    int64_t connected_since_us;
} wifi_conn_info_t;

typedef struct {
    uint32_t ip; /* network byte order, as in esp_ip4_addr_t */
    uint8_t mac[6];
    bool is_self;
    bool is_gateway;
} lan_host_t;

void wifi_mgr_init(void);
esp_netif_t *wifi_mgr_netif(void);
/* The single SoftAP netif, created on first use (shared by the repeater and rogue AP). */
esp_netif_t *wifi_mgr_ap_netif(void);

/* Re-applies station/SoftAP MAC addresses: briefly stops the radio, sets the
   MACs and reconnects the station to the last network. Used by MAC spoofing. */
bool wifi_mgr_apply_mac(const uint8_t sta[6], const uint8_t ap[6]);

/* Access point scanning */
bool wifi_mgr_scan_start(void);
bool wifi_mgr_scanning(void);
uint32_t wifi_mgr_scan_gen(void); /* bumps on each completed scan */
int wifi_mgr_get_aps(wifi_ap_record_t *out, int max);
int wifi_mgr_ap_count(void);

/* Station connection */
void wifi_mgr_connect(const char *ssid, const char *password);
void wifi_mgr_disconnect(void);
wifi_state_t wifi_mgr_state(void);
const char *wifi_mgr_state_text(void);
bool wifi_mgr_conn_info(wifi_conn_info_t *out);
/* SSID of the network being (or last) connected to; false if none yet. */
bool wifi_mgr_target_ssid(char *ssid, size_t len);
/* Connects to the most recently used saved network, if any. */
bool wifi_mgr_autoconnect(void);

/* Saved networks (stored in NVS on successful connection) */
bool wifi_mgr_known_password(const char *ssid, char *pass_out, size_t len);
int wifi_mgr_known_count(void);
void wifi_mgr_forget_all(void);

/* LAN host discovery via ARP sweep of the local /24 */
bool wifi_mgr_lan_scan_start(void);
bool wifi_mgr_lan_scanning(int *progress_pct);
uint32_t wifi_mgr_lan_gen(void);
int wifi_mgr_lan_hosts(lan_host_t *out, int max);
int wifi_mgr_lan_host_count(void);

/* Helpers shared by the UI */
const char *wifi_auth_str(wifi_auth_mode_t mode);
const char *wifi_cipher_str(wifi_cipher_type_t c);
int wifi_ap_bandwidth_mhz(const wifi_ap_record_t *ap);
/* Center channel of the occupied band (differs from primary for 40 MHz). */
float wifi_ap_center_channel(const wifi_ap_record_t *ap);
int wifi_rssi_quality(int rssi); /* 0..100 % */
