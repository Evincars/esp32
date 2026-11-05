#include "netid.h"

#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "wifi_mgr.h"

static const char *TAG = "netid";

#define NVS_NS "netid"
#define NVS_KEY "mac"

/* A handful of genuine HP OUIs; the vendor lookup (oui.cpp) and any external
   scanner will read "Hewlett Packard" from the first three bytes. */
static const uint8_t HP_OUIS[][3] = {
    {0x3C, 0xD9, 0x2B}, {0x98, 0xE7, 0xF4}, {0xA0, 0x48, 0x1C},
    {0x70, 0x5A, 0x0F}, {0x94, 0x57, 0xA5}, {0xD4, 0x85, 0x64},
};

typedef struct {
    bool enabled;
    uint8_t sta[6];
    uint8_t ap[6];
    uint8_t bt[6];
} identity_t;

static identity_t s_id;

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    identity_t id;
    size_t len = sizeof(id);
    if (nvs_get_blob(h, NVS_KEY, &id, &len) == ESP_OK && len == sizeof(id)) s_id = id;
    nvs_close(h);
}

static void store(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, &s_id, sizeof(s_id));
    nvs_commit(h);
    nvs_close(h);
}

/* Fresh HP-looking address: fixed HP OUI + 3 random bytes. The OUI's first byte
   already has the unicast/universal bits clear, which the Wi-Fi driver requires. */
static void gen_hp_mac(const uint8_t oui[3], uint8_t out[6])
{
    memcpy(out, oui, 3);
    out[3] = esp_random() & 0xFF;
    out[4] = esp_random() & 0xFF;
    out[5] = esp_random() & 0xFF;
}

/* Pushes the stored MACs into the MAC provider so esp_read_mac()/the Wi-Fi and
   BT stacks pick them up at interface init. Safe to call before esp_wifi_init(). */
static void apply_overrides(void)
{
    if (!s_id.enabled) return;
    esp_iface_mac_addr_set(s_id.sta, ESP_MAC_WIFI_STA);
    esp_iface_mac_addr_set(s_id.ap, ESP_MAC_WIFI_SOFTAP);
    esp_iface_mac_addr_set(s_id.bt, ESP_MAC_BT);
}

void netid_init(void)
{
    load();
    apply_overrides();
    if (s_id.enabled) ESP_LOGI(TAG, "spoofed MAC identity active");
}

bool netid_spoofed(void)
{
    return s_id.enabled;
}

void netid_mac(esp_mac_type_t type, uint8_t out[6])
{
    if (s_id.enabled) {
        if (type == ESP_MAC_WIFI_SOFTAP) memcpy(out, s_id.ap, 6);
        else if (type == ESP_MAC_BT) memcpy(out, s_id.bt, 6);
        else memcpy(out, s_id.sta, 6);
        return;
    }
    esp_read_mac(out, type);
}

const char *netid_label(void)
{
    return s_id.enabled ? "HP (spoofed)" : "factory";
}

bool netid_randomize(void)
{
    const uint8_t *oui = HP_OUIS[esp_random() % (sizeof(HP_OUIS) / sizeof(HP_OUIS[0]))];
    identity_t id = {};
    id.enabled = true;
    gen_hp_mac(oui, id.sta);
    gen_hp_mac(oui, id.ap);
    gen_hp_mac(oui, id.bt);
    /* the STA and AP MACs must differ */
    id.ap[5] ^= 0x22;
    id.bt[5] ^= 0x44;

    s_id = id;
    store();
    apply_overrides();
    ESP_LOGW(TAG, "MAC identity randomised to HP %02X:%02X:%02X:xx", oui[0], oui[1], oui[2]);
    return wifi_mgr_apply_mac(s_id.sta, s_id.ap);
}

bool netid_restore(void)
{
    s_id.enabled = false;
    store();
    uint8_t sta[6], ap[6], bt[6];
    esp_read_mac(sta, ESP_MAC_WIFI_STA);
    esp_read_mac(ap, ESP_MAC_WIFI_SOFTAP);
    esp_read_mac(bt, ESP_MAC_BT);
    esp_iface_mac_addr_set(sta, ESP_MAC_WIFI_STA);
    esp_iface_mac_addr_set(ap, ESP_MAC_WIFI_SOFTAP);
    esp_iface_mac_addr_set(bt, ESP_MAC_BT);
    return wifi_mgr_apply_mac(sta, ap);
}
