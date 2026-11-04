#pragma once

#include <cstdint>

/* Vendor name for a MAC address from a small built-in OUI table, or null if unknown.
   Locally administered (randomized) addresses return "Private". */
const char *oui_vendor(const uint8_t mac[6]);
