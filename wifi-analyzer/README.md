# WiFi Toolkit for LilyGO T-HMI

Touch-driven Wi-Fi analyzer for the LilyGO T-HMI (ESP32-S3, 2.8" ST7789 320x240, XPT2046 resistive touch).
Built with ESP-IDF 6.1 and LVGL 9.2.

## Features

- **Scanner** - nearby access points with SSID, BSSID (MAC), channel, bandwidth, RSSI and security.
  Sort by signal/channel/name. Tap a network for full details (cipher, PHY modes, WPS, country, BSS color) and to connect.
- **Channel graph** - every AP drawn as a curve over channels 1-13, so overlapping networks are easy to spot.
  Tap a curve to highlight it. The strip underneath shows per-channel load and the least busy of channels 1/6/11.
- **Network** - IPv4/netmask/gateway/DNS, IPv6 link-local, MAC, hostname, live RSSI chart, and a LAN host
  discovery (ARP sweep of the local /24).
- **On-screen keyboard** for Wi-Fi passwords (show/hide toggle, hidden SSID support). Successful networks are saved
  and auto-joined at boot.
- **System** - chip, memory, die temperature, battery, uptime, firmware versions.
- **Settings** - brightness, auto-connect, scan interval, touch calibration, forget saved networks.
- **About** - author and website with a QR code.

## Build & flash

```sh
idf.py set-target esp32s3   # once
idf.py build flash monitor
```

LVGL is fetched automatically by the component manager (`main/idf_component.yml`).

## First boot

The touch panel has to be calibrated once: tap the three cyan targets, then the green one to verify.
The calibration is stored in NVS. To redo it, use **Settings -> Calibrate**, or hold the **BOOT** button
while the splash screen is shown.

The **BOOT** button also works as "back" on every screen.

## Tweaks

If the picture is mirrored or the colors look inverted, change `LCD_MIRROR_X`, `LCD_MIRROR_Y` or
`LCD_INVERT_COLOR` in `main/board.h`.

## Layout

| Path | What |
| --- | --- |
| `main/board.*` | pins, ST7789 over i80, backlight PWM, battery ADC |
| `main/touch.*` | XPT2046 driver, 3-point affine calibration |
| `main/lvgl_port.*` | LVGL display/input glue and render task |
| `main/wifi_mgr.*` | scanning, connecting, saved networks, LAN discovery |
| `main/ui/` | one file per screen, shared widgets in `ui_common.cpp` |
