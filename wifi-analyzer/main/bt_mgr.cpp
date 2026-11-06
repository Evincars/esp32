#include "bt_mgr.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "bt";

/* Nordic UART Service UUIDs (little-endian byte order for BLE_UUID128_INIT). */
static const ble_uuid128_t NUS_SVC =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX = /* phone -> device (write) */
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX = /* device -> phone (notify) */
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static bool s_ready;
static uint8_t s_own_addr_type;
static SemaphoreHandle_t s_lock;

/* scan */
static bt_dev_t s_devs[BT_MAX_DEVS];
static int s_ndev;
static volatile bool s_scanning;
static volatile uint32_t s_scan_gen;

/* peripheral / NUS */
static volatile bool s_periph;
static volatile bool s_connected;
static uint16_t s_conn_handle;
static uint16_t s_tx_handle;
static char s_name[32] = "esp32-s3-ble";
static char s_rx[BT_RX_BUF];
static size_t s_rx_len;
static volatile uint32_t s_rx_gen;

/* broadcast */
static volatile bool s_broadcast;
static volatile uint32_t s_bcast_count;
static TaskHandle_t s_bcast_task;

struct Lock {
    Lock() { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(s_lock); }
};

/* ---------------- GATT server (NUS) ---------------- */

static int gatt_access(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len) == 0 && len) {
            Lock l;
            /* keep the newest text if the console fills up */
            if (s_rx_len + len + 1 >= sizeof(s_rx)) {
                size_t drop = sizeof(s_rx) / 2;
                memmove(s_rx, s_rx + drop, s_rx_len - drop);
                s_rx_len -= drop;
            }
            memcpy(s_rx + s_rx_len, buf, len);
            s_rx_len += len;
            if (s_rx_len == 0 || s_rx[s_rx_len - 1] != '\n') s_rx[s_rx_len++] = '\n';
            s_rx[s_rx_len] = 0;
            s_rx_gen = s_rx_gen + 1;
        }
    }
    return 0;
}

static struct ble_gatt_chr_def s_chrs[3];
static struct ble_gatt_svc_def s_svcs[2];

static void gatt_build(void)
{
    s_chrs[0].uuid = &NUS_RX.u;
    s_chrs[0].access_cb = gatt_access;
    s_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    s_chrs[1].uuid = &NUS_TX.u;
    s_chrs[1].access_cb = gatt_access;
    s_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
    s_chrs[1].val_handle = &s_tx_handle;
    /* s_chrs[2] left zeroed = terminator */

    s_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_svcs[0].uuid = &NUS_SVC.u;
    s_svcs[0].characteristics = s_chrs;
    /* s_svcs[1] left zeroed = terminator */
}

/* ---------------- advertising ---------------- */

static int periph_gap_event(struct ble_gap_event *event, void *);

static void advertise_nus(void)
{
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;
    fields.uuids128 = &NUS_SVC;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        /* name + 128-bit UUID can overflow 31 bytes; fall back to name only */
        struct ble_hs_adv_fields f2 = {};
        f2.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        f2.name = (uint8_t *)s_name;
        f2.name_len = strlen(s_name);
        f2.name_is_complete = 1;
        ble_gap_adv_set_fields(&f2);
    }

    struct ble_gap_adv_params adv = {};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &adv, periph_gap_event, nullptr);
}

static int periph_gap_event(struct ble_gap_event *event, void *)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
        } else if (s_periph) {
            advertise_nus();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        if (s_periph) advertise_nus();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_periph) advertise_nus();
        break;
    default:
        break;
    }
    return 0;
}

/* ---------------- scanning (observer) ---------------- */

static int find_dev(const uint8_t be_addr[6])
{
    for (int i = 0; i < s_ndev; i++)
        if (memcmp(s_devs[i].addr, be_addr, 6) == 0) return i;
    return -1;
}

static void handle_report(const struct ble_gap_disc_desc *d)
{
    uint8_t be[6];
    for (int i = 0; i < 6; i++) be[i] = d->addr.val[5 - i]; /* val is little-endian */

    struct ble_hs_adv_fields f;
    bool parsed = ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0;

    Lock l;
    int idx = find_dev(be);
    if (idx < 0) {
        if (s_ndev >= BT_MAX_DEVS) return;
        idx = s_ndev++;
        memset(&s_devs[idx], 0, sizeof(s_devs[idx]));
        memcpy(s_devs[idx].addr, be, 6);
    }
    bt_dev_t &e = s_devs[idx];
    e.addr_type = d->addr.type == BLE_ADDR_PUBLIC ? 0 : 1;
    e.random_private = d->addr.type == BLE_ADDR_RANDOM && (d->addr.val[5] & 0xC0) != 0xC0;
    e.rssi = d->rssi;
    if (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND)
        e.connectable = true;
    e.last_seen_us = esp_timer_get_time();

    if (parsed) {
        if (f.name && f.name_len) {
            int n = std::min<int>(f.name_len, (int)sizeof(e.name) - 1);
            memcpy(e.name, f.name, n);
            e.name[n] = 0;
        }
        if (f.appearance_is_present) e.appearance = f.appearance;
        if (f.mfg_data && f.mfg_data_len >= 2) e.company = f.mfg_data[0] | (f.mfg_data[1] << 8);
        else e.company = 0xFFFF;
        e.n_svc16 = 0;
        for (int i = 0; i < f.num_uuids16 && e.n_svc16 < 4; i++) e.svc16[e.n_svc16++] = f.uuids16[i].value;
    }
    /* keep the latest raw report for the detail view */
    int cp = std::min<int>(d->length_data, (int)sizeof(e.adv));
    memcpy(e.adv, d->data, cp);
    e.adv_len = cp;

    s_scan_gen = s_scan_gen + 1;
}

static int disc_gap_event(struct ble_gap_event *event, void *)
{
    if (event->type == BLE_GAP_EVENT_DISC) handle_report(&event->disc);
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) s_scanning = false;
    return 0;
}

bool bt_scan_start(void)
{
    if (!s_ready || s_scanning || s_broadcast) return false;
    struct ble_gap_disc_params p = {};
    p.passive = 0;             /* active: also pull scan responses (names) */
    p.filter_duplicates = 0;   /* keep RSSI fresh */
    p.itvl = 0;
    p.window = 0;
    if (ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, disc_gap_event, nullptr) != 0) return false;
    s_scanning = true;
    return true;
}

void bt_scan_stop(void)
{
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    s_scanning = false;
}

bool bt_scanning(void) { return s_scanning; }
uint32_t bt_scan_gen(void) { return s_scan_gen; }

int bt_scan_get(bt_dev_t *out, int max)
{
    Lock l;
    int n = std::min(max, s_ndev);
    memcpy(out, s_devs, n * sizeof(bt_dev_t));
    return n;
}

int bt_scan_count(void) { return s_ndev; }

/* ---------------- peripheral ---------------- */

bool bt_peripheral_start(const char *name)
{
    if (!s_ready || s_broadcast) return false;
    if (name && name[0]) {
        strlcpy(s_name, name, sizeof(s_name));
        ble_svc_gap_device_name_set(s_name);
    }
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    s_periph = true;
    advertise_nus();
    ESP_LOGI(TAG, "advertising NUS as '%s'", s_name);
    return true;
}

void bt_peripheral_stop(void)
{
    s_periph = false;
    if (s_connected) ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (ble_gap_adv_active()) ble_gap_adv_stop();
}

bool bt_peripheral_running(void) { return s_periph; }
bool bt_connected(void) { return s_connected; }
const char *bt_peripheral_name(void) { return s_name; }

void bt_peripheral_send(const char *text)
{
    if (!s_connected || !text || !text[0]) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (om) ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
}

int bt_rx_text(char *out, size_t max)
{
    Lock l;
    size_t n = std::min(max - 1, s_rx_len);
    memcpy(out, s_rx, n);
    out[n] = 0;
    return (int)n;
}

uint32_t bt_rx_gen(void) { return s_rx_gen; }

/* ---------------- broadcast (RF airtime test) ---------------- */

static void bcast_task(void *)
{
    uint8_t mfg[8] = {0xE5, 0x02}; /* arbitrary company id + counter, benign payload */
    while (s_broadcast) {
        if (ble_gap_adv_active()) ble_gap_adv_stop();

        /* fresh static-random address each cycle (top two bits set) */
        uint8_t addr[6];
        esp_fill_random(addr, 6);
        addr[5] |= 0xC0;
        ble_hs_id_set_rnd(addr);

        for (int i = 2; i < 8; i++) mfg[i] = esp_random() & 0xFF;
        struct ble_hs_adv_fields fields = {};
        fields.flags = BLE_HS_ADV_F_BREDR_UNSUP;
        fields.mfg_data = mfg;
        fields.mfg_data_len = sizeof(mfg);
        char nm[16];
        snprintf(nm, sizeof(nm), "RF-%04lX", (unsigned long)(s_bcast_count & 0xFFFF));
        fields.name = (uint8_t *)nm;
        fields.name_len = strlen(nm);
        fields.name_is_complete = 1;
        ble_gap_adv_set_fields(&fields);

        struct ble_gap_adv_params adv = {};
        adv.conn_mode = BLE_GAP_CONN_MODE_NON; /* non-connectable */
        adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
        adv.itvl_min = adv.itvl_max = 0x30; /* ~30 ms */
        if (ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, nullptr, BLE_HS_FOREVER, &adv, periph_gap_event, nullptr) == 0)
            s_bcast_count = s_bcast_count + 1;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    s_bcast_task = nullptr;
    vTaskDelete(nullptr);
}

bool bt_broadcast_start(void)
{
    if (!s_ready || s_broadcast) return false;
    if (s_scanning) bt_scan_stop();
    if (s_periph) bt_peripheral_stop();
    s_bcast_count = 0;
    s_broadcast = true;
    if (xTaskCreate(bcast_task, "bt_bcast", 3072, nullptr, 4, &s_bcast_task) != pdPASS) {
        s_broadcast = false;
        return false;
    }
    ESP_LOGW(TAG, "BLE broadcast test running");
    return true;
}

void bt_broadcast_stop(void)
{
    s_broadcast = false;
    while (s_bcast_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool bt_broadcast_running(void) { return s_broadcast; }
uint32_t bt_broadcast_count(void) { return s_bcast_count; }

/* ---------------- host bring-up ---------------- */

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_ready = true;
    ESP_LOGI(TAG, "BLE ready");
}

static void on_reset(int reason)
{
    s_ready = false;
    ESP_LOGW(TAG, "BLE reset, reason %d", reason);
}

static void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void bt_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    gatt_build();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_svc_gap_device_name_set(s_name);

    nimble_port_freertos_init(host_task);
}

bool bt_ready(void) { return s_ready; }

/* ---------------- helpers ---------------- */

const char *bt_company_name(uint16_t id)
{
    switch (id) {
    case 0x0006: return "Microsoft";
    case 0x004C: return "Apple";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0059: return "Nordic";
    case 0x0499: return "Ruuvi";
    case 0x0157: return "Huawei";
    case 0x038F: return "Xiaomi";
    case 0x02E5: return "Espressif";
    case 0x0171: return "Amazon";
    case 0x0087: return "Garmin";
    case 0x00D2: return "Fitbit";
    case 0x0001: return "Ericsson";
    case 0x000F: return "Broadcom";
    case 0xFFFF: return "";
    default: return "";
    }
}

const char *bt_appearance_name(uint16_t a)
{
    switch (a >> 6) { /* category = top 10 bits */
    case 1: return "Phone";
    case 2: return "Computer";
    case 3: return "Watch";
    case 5: return "Display";
    case 7: return "Tag";
    case 8: return "Keyring";
    case 9: return "Media player";
    case 10: return "Barcode";
    case 64: return "Wearable";
    case 66: return "Sensor";
    case 81: return "Heart rate";
    case 0x21: return "Headset";
    case 0x22: return "Headphones";
    case 0x23: return "Speaker";
    default: return a ? "Device" : "";
    }
}

const char *bt_classify(const bt_dev_t *d)
{
    const char *ap = bt_appearance_name(d->appearance);
    if (ap[0]) return ap;
    for (int i = 0; i < d->n_svc16; i++) {
        switch (d->svc16[i]) {
        case 0x180D: return "Heart rate";
        case 0x180F: return "Battery";
        case 0x1812: return "HID input";
        case 0xFE9F: return "Google";
        case 0xFD6F: return "Exposure tag";
        case 0xFEAA: return "Eddystone";
        }
    }
    if (d->company == 0x004C) return "Apple device";
    if (d->company == 0x0006) return "Microsoft";
    if (!d->connectable) return "Beacon";
    return "";
}
