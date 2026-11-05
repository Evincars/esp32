# WiFi Toolkit for LilyGO T-HMI

Touch-driven Wi-Fi and network analyzer for the LilyGO T-HMI (ESP32-S3, 2.8" ST7789 320x240, XPT2046 resistive touch).
Built with ESP-IDF 6.1 and LVGL 9.2.

## Features

- **Scanner** - nearby access points with SSID, BSSID (MAC), channel, bandwidth, RSSI and security.
  Sort by signal/channel/name. Tap a network for full details (cipher, PHY modes, WPS, country, BSS color) and to connect.
- **Channel graph** - every AP drawn as a curve over channels 1-13, so overlapping networks are easy to spot.
  Tap a curve to highlight it. The strip underneath shows per-channel load and the least busy of channels 1/6/11.
- **Network** - IPv4/netmask/gateway/DNS, IPv6 link-local, MAC, hostname, live RSSI chart, and LAN host
  discovery (ARP sweep of the local /24) with MAC vendor names. Tap a host to port-scan it.
- **Port scan** - nmap-style TCP connect scan of one host, a custom IP or every discovered host. Profiles: ~75
  well-known service ports or 1-1024. Open ports are probed for a banner (SSH/FTP/SMTP greeting, HTTP `Server`
  header, RTSP, MySQL version).
- **Sniffer** - packet capture with a capture filter (ARP, ICMP, TCP, UDP, DNS, DHCP, HTTP/TLS, 802.11 mgmt, EAPOL).
  - *LAN* mode: every frame this board sends or receives plus the network's broadcast/multicast traffic
    (ARP, DHCP, mDNS, SSDP, NetBIOS...), decoded up to the application layer.
  - *802.11* mode: monitor mode on one channel (beacons, probes, deauth, data, WPA handshakes). Other stations'
    data is encrypted on WPA networks, so it decodes up to the 802.11 header.
  - Tap a frame for a Wireshark-style breakdown by OSI layer (L1 radio/link, L2 Ethernet/802.11/LLC, L3 IPv4/IPv6/ARP/ICMP,
    L4 TCP/UDP, L6 TLS with SNI/ALPN, L7 DNS/mDNS/NBNS/LLMNR/DHCP/HTTP/SSDP/NTP/MQTT/QUIC/SSH) and a hex dump.
- **Repeater** - range extender: stays joined to the home Wi-Fi as a station and opens its own SoftAP,
  forwarding client traffic through a custom NAT (lwIP NAPT) so Wi-Fi reaches the next room. Editable SSID
  (default `repeater-esp32-s3`) and password (default `hesloheslo`, blank = open), per-side throughput counters
  (bytes to the home router vs. through the repeater), SoftAP gateway IP and a live list of connected clients
  (IP, MAC, vendor, RSSI). Also randomizes all three radio MACs (Wi-Fi STA, Wi-Fi AP, BLE) to a fresh HP-OUI
  identity, or restores the factory MACs; the choice is stored and re-applied at boot.
- **Web server** - HTTP dashboard on port 80 (QR code on the device) for any phone/PC on the same network: system
  metrics with a history chart, Wi-Fi details, AP table, LAN hosts, port scan results and a live packet list with
  per-layer detail. Read-only, plus a "Rescan" button. Optionally started at boot.
- **System monitor** - CPU load per core, die temperature (with min/max), battery voltage, internal RAM, PSRAM,
  app flash usage, 60 s history chart, chip and firmware details.
- **Clock** - NTP time with a 60-tick seconds ring; time zone in Settings.
- **Settings** - color palette (Graphite, Midnight, Ember), brightness, auto-connect, scan interval, time zone,
  web dashboard at boot, RST shortcuts, touch calibration, forget saved networks, sleep now.
- **About** - author and website with a QR code.

### RF lab / offensive tools

> ⚠️ **Legal & ethical warning.** The tools below transmit raw 802.11 frames, impersonate networks and
> intercept traffic. Only use them on radios, networks and devices **you own or are explicitly authorised to
> test**. Jamming, deauthenticating or phishing third parties is illegal in most jurisdictions. Each tool is
> gated behind an on-device confirmation dialog. You are responsible for how you use this firmware.

- **Deauth** - sends 802.11 deauthentication frames to a chosen AP (broadcast or a selected BSSID) to test how
  resilient nearby clients are to disconnection. Pick a scanned AP, confirm, start/stop; shows frames sent and pps.
- **Beacon spam** - broadcasts hundreds of fake access-point beacons (configurable count) so the area fills with
  bogus SSIDs. Confirm-gated; shows the beacon rate.
- **Rogue AP** - opens an open "free Wi-Fi" access point with a captive portal. A built-in DNS redirector points
  every lookup at the board, and a spoofed login page captures the credentials visitors type (social-engineering
  simulation). Editable SSID, live client/DNS/HTTP counters and a captured-credentials list. Starting it stops the
  web dashboard (both want port 80).
- **CSI motion** - uses Wi-Fi Channel State Information to sense motion/presence in the room without a camera. The
  per-subcarrier amplitude is diffed frame-to-frame on the ESP32-S3 vector/SIMD unit (esp-dsp `dsps_sub_f32` /
  `dsps_dotprod_f32`) to compute a motion-energy percentage, a presence flag and a live waveform. Adjustable
  sensitivity. Needs an associated AP so CSI frames keep arriving.
- **Locator (FTM)** - measures the distance to an FTM/RTT-capable router with the 802.11mc Fine Timing
  Measurement standard and plots it on a radar with a direction vector. Distance is centimetre-class; the bearing
  is an RSSI-trend estimate (there is no compass/antenna array), so treat the arrow as a warmer/colder hint.

## Buttons

| Button | Action |
| --- | --- |
| **PWR / USER** (GPIO21) | hibernate: turns the display and backlight off while the firmware keeps running in the background (Wi-Fi, web server and any running tool stay up). Press again to wake. |
| **BOOT** (GPIO0) | on the top-level views, cycles menu -> clock -> system monitor; on any other screen, goes back. Also wakes the board from hibernate/sleep. |
| **RST**, once | reboot into the next view: menu -> system monitor -> clock -> menu |
| **RST**, twice within ~2 s | deep sleep (display and touch powered off); BOOT or RST wakes it |

RST resets the chip, so the firmware only sees a fresh boot. The ESP32-S3 reports an RST press and a real
power-on the same way, so turning the board on also advances the view. The view that is on screen is
remembered, so if you are on the menu the next RST always shows the system monitor. The shortcuts can be turned
off in **Settings -> RST shortcuts**. Resets from flashing over USB always start at the menu.

## Build & flash

```sh
idf.py set-target esp32s3   # once
idf.py build flash monitor
```

LVGL is fetched automatically by the component manager (`main/idf_component.yml`).

## First boot

The touch panel has to be calibrated once: tap the three targets, then the last one to verify.
The calibration is stored in NVS. To redo it, use **Settings -> Calibrate**, or hold the **BOOT** button
while the splash screen is shown.

## Tweaks

If the picture is mirrored or the colors look inverted, change `LCD_MIRROR_X`, `LCD_MIRROR_Y` or
`LCD_INVERT_COLOR` in `main/board.h`.

## Layout

| Path | What |
| --- | --- |
| `main/board.*` | pins, ST7789 over i80, backlight PWM, battery ADC, deep sleep |
| `main/touch.*` | XPT2046 driver, 3-point affine calibration |
| `main/lvgl_port.*` | LVGL display/input glue and render task |
| `main/wifi_mgr.*` | scanning, connecting, saved networks, LAN discovery |
| `main/boot_mode.*` | RST shortcuts (boot view, double press to sleep) |
| `main/sysmon.*` | 1 Hz sampling of CPU load, temperature, memory, battery |
| `main/time_sync.*` | SNTP and time zones |
| `main/portscan.*` | TCP connect scanner and banner grabbing |
| `main/sniffer.*` | LAN (lwIP netif hook) and 802.11 (monitor mode) capture into a PSRAM ring |
| `main/wifi_attack.*` | raw-frame deauth flood and beacon spam engine |
| `main/rogue_ap.*` | open softAP, captive DNS redirector and credential-capturing portal |
| `main/repeater.*` | APSTA range extender: NAPT router, per-side byte counters, client list |
| `main/netid.*` | MAC identity: randomize STA/AP/BT MACs to an HP OUI, persisted and re-applied at boot |
| `main/wifi_csi.*` | CSI capture + esp-dsp SIMD motion/presence detection |
| `main/wifi_ftm.*` | 802.11mc Fine Timing Measurement distance sessions |
| `main/pkt_decode.*` | protocol dissectors: summary line and per-OSI-layer fields |
| `main/oui.*` | small MAC vendor table |
| `main/web_server.*`, `main/web/index.html` | HTTP dashboard and JSON API |
| `main/ui/` | one file per screen, shared widgets and the palettes in `ui_common.cpp` |

## JSON API

All `GET`, served by the dashboard: `/api/status`, `/api/aps`, `/api/hosts`, `/api/ports`,
`/api/packets?since=<seq>`, `/api/packet?seq=<seq>`. `POST /api/scan` starts a Wi-Fi scan.
