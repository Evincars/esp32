#pragma once

#include <cstddef>
#include <cstdint>

/* Bluetooth Low Energy (NimBLE) for home/diagnostic use, coexisting with Wi-Fi:
   - Scanner: observe nearby advertisers (name, address, RSSI, vendor, appearance,
     services), classify them and flag randomized (private) addresses.
   - Peripheral: advertise a Nordic UART Service so a phone can connect and exchange
     text both ways.
   - Broadcast: a non-connectable advertising airtime/RF test (authorized use only),
     with a benign payload - not pop-up/pairing spam.
   The ESP32-S3 has BLE only (no Bluetooth Classic). */

#define BT_MAX_DEVS 48
#define BT_RX_BUF 1024

typedef struct {
    uint8_t addr[6];     /* big-endian display order */
    uint8_t addr_type;   /* 0 public, 1 random */
    bool random_private; /* resolvable/non-resolvable private (randomized) address */
    int8_t rssi;
    bool connectable;
    char name[32];
    uint16_t company;    /* manufacturer company id, 0xFFFF if none */
    uint16_t appearance; /* 0 if none */
    uint16_t svc16[4];
    int n_svc16;
    uint8_t adv[62]; /* raw advertising + scan-response data */
    int adv_len;
    int64_t last_seen_us;
} bt_dev_t;

void bt_init(void);
bool bt_ready(void);

/* ---------------- scan (observer) ---------------- */
bool bt_scan_start(void);
void bt_scan_stop(void);
bool bt_scanning(void);
uint32_t bt_scan_gen(void); /* bumps when the device table changes */
int bt_scan_get(bt_dev_t *out, int max);
int bt_scan_count(void);

/* ---------------- peripheral (Nordic UART Service) ---------------- */
bool bt_peripheral_start(const char *name);
void bt_peripheral_stop(void);
bool bt_peripheral_running(void);
bool bt_connected(void);
const char *bt_peripheral_name(void);
/* Notifies `text` to the connected central over the NUS TX characteristic. */
void bt_peripheral_send(const char *text);
/* Console of text received from the central; returns bytes copied. */
int bt_rx_text(char *out, size_t max);
uint32_t bt_rx_gen(void);

/* ---------------- broadcast (RF airtime test) ---------------- */
bool bt_broadcast_start(void);
void bt_broadcast_stop(void);
bool bt_broadcast_running(void);
uint32_t bt_broadcast_count(void);

/* ---------------- helpers ---------------- */
const char *bt_company_name(uint16_t id);
const char *bt_appearance_name(uint16_t appearance);
/* Human device category from appearance + advertised services, e.g. "Phone". */
const char *bt_classify(const bt_dev_t *d);
