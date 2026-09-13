# ESP32 serial log display

This ESP-IDF application receives text through ESP32 UART2 and displays it on
an ILI9488-based 480x320 SPI TFT. It uses separate FreeRTOS tasks for UART
reception and display rendering, with a 4 KiB stream buffer between them.

The terminal is 80 columns by 40 rows. It supports wrapping, scrolling, CR,
LF, tabs, backspace, printable ASCII, and discards common ANSI CSI sequences.

## Wiring

### TFT to ESP32 DevKit V1

| TFT | ESP32 |
| --- | --- |
| CS | GPIO 15 |
| RESET | GPIO 4 |
| DC/RS | GPIO 2 |
| MOSI/SDI | GPIO 23 |
| SCK | GPIO 18 |
| GND | GND |

Connect the display power and backlight pins according to the voltage labels
on the particular module. GPIO 2 and GPIO 15 are ESP32 strapping pins; the TFT
must not force them to an invalid level while the ESP32 is resetting.

### USB-TTL adapter to ESP32

| USB-TTL | ESP32 |
| --- | --- |
| TX | GPIO 16 (UART2 RX) |
| RX | GPIO 17 (UART2 TX, optional) |
| GND | GND |

Use a **3.3 V TTL** adapter. Do not connect RS-232 voltage levels or a 5 V TTL
TX signal directly to the ESP32. The adapter's VCC pin is not needed when the
ESP32 is powered separately.

## Build and flash

In the VS Code ESP-IDF extension:

1. Set the target to `esp32`.
2. Select the serial port belonging to the ESP32 DevKit, not the USB-TTL input.
3. Run **Build, Flash and Monitor**.

After reset, the TFT should show:

```text
Serial console ready - UART2 115200 8N1
```

## Send Linux output

Replace `/dev/ttyUSB0` with the USB-TTL adapter's device. First configure it:

```bash
stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo
```

Send a fixed command:

```bash
uname -a > /dev/ttyUSB0
```

Follow the system journal:

```bash
journalctl -f -o short-monotonic --no-pager > /dev/ttyUSB0
```

Follow one service:

```bash
journalctl -fu ssh.service -o short-monotonic --no-pager > /dev/ttyUSB0
```

If access is denied, add your Linux account to the serial-port group (often
`dialout`) and sign out and back in. Avoid running an interactive serial
monitor on the USB-TTL port while redirecting data to it.

## System monitor graphs

`system_monitor.py` reads CPU per-core usage, CPU temperature, RAM usage,
RAM temperature (if exposed by a sensor), and NVIDIA VRAM/GPU usage/temperature,
then streams one stats line per interval to the ESP32 over the same serial
port used above. The firmware detects these lines automatically and switches
the screen to bar-graph gauges with labels and numeric values; sending plain
text (like `ping` output or `journalctl -f`) switches it back to the scrolling
terminal view.

```bash
pip install psutil pyserial
stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo
python3 system_monitor.py --port /dev/ttyUSB0 --interval 1
```

Metrics that can't be read on a given machine (e.g. RAM temperature, or GPU
stats without an NVIDIA card) are sent as empty values and shown as `N/A`.

Wire format, one ASCII line terminated by `\n`:

```text
#SYS#cpus=12.3;45.0;10.0;99.9|cputemp=61.5|ramused=8192|ramtotal=16384|ramtemp=|vramused=2048|vramtotal=8192|gpuusage=33|gputemp=55#END#
```

## Display troubleshooting

The program uses an ILI9488 initialization sequence and 18-bit SPI pixel data.
If the backlight is on but the startup banner does not appear, check the
controller marking or seller documentation. Similar 3.5-inch boards may use
an ILI9486, HX8357, or another controller and require a different init sequence.

If the image is mirrored or rotated, adjust `memory_access` in
`lcd_initialize()`; its current ILI9488 MADCTL value is `0xe8` for landscape.
If the display is unstable on long jumper wires, reduce `clock_speed_hz` from
40 MHz to 20 MHz.