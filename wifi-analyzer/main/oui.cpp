#include "oui.h"

#include <cstddef>

typedef struct {
    uint32_t oui; /* first three bytes */
    const char *vendor;
} oui_t;

/* A short list of vendors common on home/office networks. Far from complete:
   unknown prefixes simply show no vendor. */
static const oui_t OUIS[] = {
    /* Espressif (ESP8266 / ESP32 IoT gear: Shelly, Tasmota, Tuya...) */
    {0x18FE34, "Espressif"}, {0x240AC4, "Espressif"}, {0x246F28, "Espressif"}, {0x2462AB, "Espressif"},
    {0x30AEA4, "Espressif"}, {0x3C71BF, "Espressif"}, {0x5CCF7F, "Espressif"}, {0x600194, "Espressif"},
    {0x7C9EBD, "Espressif"}, {0x840D8E, "Espressif"}, {0x84CCA8, "Espressif"}, {0x84F3EB, "Espressif"},
    {0x8CAAB5, "Espressif"}, {0x94B97E, "Espressif"}, {0xA4CF12, "Espressif"}, {0xAC67B2, "Espressif"},
    {0xB4E62D, "Espressif"}, {0xBCDDC2, "Espressif"}, {0xC44F33, "Espressif"}, {0xC82B96, "Espressif"},
    {0xCC50E3, "Espressif"}, {0xD8A01D, "Espressif"}, {0xDC4F22, "Espressif"}, {0xE8DB84, "Espressif"},
    {0xECFABC, "Espressif"}, {0xF4CFA2, "Espressif"}, {0x483FDA, "Espressif"}, {0x7CDFA1, "Espressif"},
    {0x348518, "Espressif"}, {0x4827E2, "Espressif"}, {0x68B6B3, "Espressif"}, {0x70041D, "Espressif"},
    {0x34B472, "Espressif"}, {0xF412FA, "Espressif"}, {0xDC5475, "Espressif"}, {0xECDA3B, "Espressif"},
    {0x64E833, "Espressif"}, {0x543204, "Espressif"},
    /* Raspberry Pi */
    {0xB827EB, "Raspberry Pi"}, {0xDCA632, "Raspberry Pi"}, {0xE45F01, "Raspberry Pi"},
    {0xD83ADD, "Raspberry Pi"}, {0x28CDC1, "Raspberry Pi"}, {0x2CCF67, "Raspberry Pi"},
    /* virtual machines */
    {0x000C29, "VMware"}, {0x005056, "VMware"}, {0x080027, "VirtualBox"}, {0x00155D, "Hyper-V"},
    /* routers and network gear */
    {0x3CA62F, "AVM"}, {0x3810D5, "AVM"}, {0xC80E14, "AVM"}, {0x444E6D, "AVM"}, {0x7CFF4D, "AVM"},
    {0x2C91AB, "AVM"}, {0xE0286D, "AVM"}, {0x989BCB, "AVM"}, {0xDC396F, "AVM"}, {0x5C4979, "AVM"},
    {0x24A43C, "Ubiquiti"}, {0x0418D6, "Ubiquiti"}, {0x44D9E7, "Ubiquiti"}, {0x687251, "Ubiquiti"},
    {0x788A20, "Ubiquiti"}, {0x802AA8, "Ubiquiti"}, {0xF09FC2, "Ubiquiti"}, {0xFCECDA, "Ubiquiti"},
    {0xB4FBE4, "Ubiquiti"}, {0x7483C2, "Ubiquiti"}, {0xE063DA, "Ubiquiti"}, {0x18E829, "Ubiquiti"},
    {0x4C5E0C, "MikroTik"}, {0x64D154, "MikroTik"}, {0x6C3B6B, "MikroTik"}, {0xB869F4, "MikroTik"},
    {0xCC2DE0, "MikroTik"}, {0xD4CA6D, "MikroTik"}, {0xE48D8C, "MikroTik"}, {0x2CC81B, "MikroTik"},
    {0x488F5A, "MikroTik"}, {0x744D28, "MikroTik"}, {0xDC2C6E, "MikroTik"}, {0x085531, "MikroTik"},
    {0x50C7BF, "TP-Link"}, {0x14CC20, "TP-Link"}, {0x98DAC4, "TP-Link"}, {0xC04A00, "TP-Link"},
    {0xF4F26D, "TP-Link"}, {0x60E327, "TP-Link"}, {0xEC086B, "TP-Link"}, {0xA42BB0, "TP-Link"},
    {0x30B5C2, "TP-Link"}, {0x54AF97, "TP-Link"}, {0xB0BE76, "TP-Link"},
    {0x00146C, "Netgear"}, {0xA040A0, "Netgear"}, {0x9C3DCF, "Netgear"}, {0xC03F0E, "Netgear"},
    {0x04D9F5, "ASUS"}, {0x2C56DC, "ASUS"}, {0x50465D, "ASUS"}, {0xAC9E17, "ASUS"},
    /* NAS, media, smart home */
    {0x001132, "Synology"}, {0x245EBE, "QNAP"}, {0x00089B, "QNAP"},
    {0x000E58, "Sonos"}, {0x5CAAFD, "Sonos"}, {0x949F3E, "Sonos"}, {0xB8E937, "Sonos"}, {0x7828CA, "Sonos"},
    {0x48A6B8, "Sonos"}, {0x542A1B, "Sonos"}, {0x347E5C, "Sonos"},
    {0x001788, "Philips Hue"}, {0xECB5FA, "Philips Hue"},
    {0xF4F5D8, "Google"}, {0xF4F5E8, "Google"}, {0x546009, "Google"}, {0x3C5AB4, "Google"},
    {0x18B430, "Nest"}, {0x641666, "Nest"},
    {0x44650D, "Amazon"}, {0xF0272D, "Amazon"}, {0x74C246, "Amazon"}, {0xFC65DE, "Amazon"},
    {0x6854FD, "Amazon"}, {0x0C47C9, "Amazon"}, {0x84D6D0, "Amazon"},
    {0xB0A737, "Roku"}, {0xDC3A5E, "Roku"},
    {0x640980, "Xiaomi"}, {0x286C07, "Xiaomi"}, {0x7811DC, "Xiaomi"}, {0x50EC50, "Xiaomi"},
    {0x7C49EB, "Xiaomi"}, {0x34CE00, "Xiaomi"},
    {0x4419B6, "Hikvision"}, {0x2857BE, "Hikvision"}, {0xC056E3, "Hikvision"}, {0xBCAD28, "Hikvision"},
    {0x008077, "Brother"}, {0x30055C, "Brother"},
    /* computers and phones with fixed MACs */
    {0x3C0754, "Apple"}, {0x705681, "Apple"}, {0xF01898, "Apple"}, {0xACBC32, "Apple"},
    {0x88665A, "Apple"}, {0xA45E60, "Apple"}, {0x8C7712, "Samsung"},
};

const char *oui_vendor(const uint8_t mac[6])
{
    if (mac[0] & 0x02) return "Private";
    uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    for (size_t i = 0; i < sizeof(OUIS) / sizeof(OUIS[0]); i++) {
        if (OUIS[i].oui == oui) return OUIS[i].vendor;
    }
    return nullptr;
}
