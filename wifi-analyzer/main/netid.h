#pragma once

#include <cstdint>

#include "esp_mac.h"

/* Network identity: the MAC addresses of the three radios (Wi-Fi station,
   Wi-Fi SoftAP and Bluetooth/BLE). By default these come from eFuse. The user
   can randomise all three to a fresh set that carries a real HP OUI, so the
   board presents itself as an HP device to anything that reads the vendor from
   the MAC. The chosen set is stored in NVS and re-applied at every boot.

   Changing the station MAC needs a brief Wi-Fi restart (and reconnect), so it
   is only allowed while the repeater's SoftAP is stopped. */

/* Call once, before wifi_mgr_init(): applies a stored spoofed identity (if any)
   so the very first Wi-Fi bring-up already uses it. */
void netid_init(void);

bool netid_spoofed(void);

/* Current desired MAC for an interface (ESP_MAC_WIFI_STA / _WIFI_SOFTAP / _BT). */
void netid_mac(esp_mac_type_t type, uint8_t out[6]);

/* "HP (spoofed)" when randomised, otherwise "factory". */
const char *netid_label(void);

/* Generate a new HP-OUI identity for all three radios, persist it and apply it
   (restarts the station link). Returns false if it could not be applied now. */
bool netid_randomize(void);

/* Restore the factory eFuse MACs, persist the choice and apply it. */
bool netid_restore(void);
