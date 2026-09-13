#!/usr/bin/env python3
"""Streams CPU/RAM/GPU stats to the ESP32 TFT monitor over a serial port.

Reads per-core CPU usage, CPU temperature, RAM usage, RAM temperature (when
exposed by a hwmon driver), and NVIDIA GPU/VRAM usage and temperature, then
sends one line per sample to the ESP32 (main.c), which renders it as graphs.
Any other text sent to the same serial port (e.g. "ping" output) is rendered
by the ESP32 as a plain scrolling log instead, so this script does not need
to be the only thing writing to the port.

Wire format (single ASCII line terminated by "\\n"):

    #SYS#cpus=<c0>;<c1>;...|cputemp=<c>|ramused=<mb>|ramtotal=<mb>|
         ramtemp=<c>|vramused=<mb>|vramtotal=<mb>|gpuusage=<pct>|gputemp=<c>#END#

A metric that can't be read is sent as an empty value (e.g. "ramtemp="); the
ESP32 side shows "N/A" for it. RAM temperature and GPU stats are frequently
unavailable depending on the machine's sensors, and that is expected.

Requirements: pip install psutil pyserial
GPU stats require NVIDIA's `nvidia-smi` on PATH; other vendors are not
supported yet and will simply show up as N/A.
"""

import argparse
import subprocess
import sys
import time
from typing import Optional, Tuple

import psutil
import serial

STATS_PREFIX = "#SYS#"
STATS_SUFFIX = "#END#"


def read_cpu_usage_percpu(interval: float):
    """Blocks for `interval` seconds while sampling per-core CPU usage."""
    return psutil.cpu_percent(interval=interval, percpu=True)


def read_cpu_temperature() -> Optional[float]:
    try:
        sensors = psutil.sensors_temperatures()
    except (AttributeError, OSError):
        return None
    for name in ("coretemp", "k10temp", "cpu_thermal", "zenpower"):
        entries = sensors.get(name)
        if entries:
            return entries[0].current
    for entries in sensors.values():
        for entry in entries:
            label = (entry.label or "").lower()
            if "package" in label or "tctl" in label or "tdie" in label:
                return entry.current
    return None


def read_ram_temperature() -> Optional[float]:
    """Looks for DIMM temperature sensors (spd5118/jc42) exposed via lm-sensors.

    Most systems do not expose this; returns None when unavailable.
    """
    try:
        sensors = psutil.sensors_temperatures()
    except (AttributeError, OSError):
        return None
    for name, entries in sensors.items():
        lname = name.lower()
        if "spd5118" in lname or "jc42" in lname or "dimm" in lname:
            if entries:
                return entries[0].current
    return None


def read_ram_usage() -> Tuple[float, float]:
    vm = psutil.virtual_memory()
    used_mb = (vm.total - vm.available) / (1024 * 1024)
    total_mb = vm.total / (1024 * 1024)
    return used_mb, total_mb


def read_nvidia_gpu() -> Tuple[Optional[float], Optional[float], Optional[float], Optional[float]]:
    """Returns (vram_used_mb, vram_total_mb, gpu_usage_pct, gpu_temp_c)."""
    query = "memory.used,memory.total,utilization.gpu,temperature.gpu"
    try:
        output = subprocess.check_output(
            ["nvidia-smi", f"--query-gpu={query}", "--format=csv,noheader,nounits"],
            stderr=subprocess.DEVNULL,
            timeout=2,
        ).decode().strip()
    except (OSError, subprocess.SubprocessError):
        return None, None, None, None

    if not output:
        return None, None, None, None
    parts = [p.strip() for p in output.splitlines()[0].split(",")]
    if len(parts) != 4:
        return None, None, None, None
    try:
        vram_used, vram_total, gpu_usage, gpu_temp = (float(p) for p in parts)
    except ValueError:
        return None, None, None, None
    return vram_used, vram_total, gpu_usage, gpu_temp


def format_value(value: Optional[float], decimals: int = 1) -> str:
    return "" if value is None else f"{value:.{decimals}f}"


def build_stats_line(interval: float) -> str:
    cpu_usage = read_cpu_usage_percpu(interval)
    cpu_temp = read_cpu_temperature()
    ram_used_mb, ram_total_mb = read_ram_usage()
    ram_temp = read_ram_temperature()
    vram_used, vram_total, gpu_usage, gpu_temp = read_nvidia_gpu()

    fields = [
        "cpus=" + ";".join(f"{v:.1f}" for v in cpu_usage),
        "cputemp=" + format_value(cpu_temp),
        "ramused=" + format_value(ram_used_mb, 0),
        "ramtotal=" + format_value(ram_total_mb, 0),
        "ramtemp=" + format_value(ram_temp),
        "vramused=" + format_value(vram_used, 0),
        "vramtotal=" + format_value(vram_total, 0),
        "gpuusage=" + format_value(gpu_usage, 0),
        "gputemp=" + format_value(gpu_temp),
    ]
    return STATS_PREFIX + "|".join(fields) + STATS_SUFFIX


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial device connected to the ESP32 (default: %(default)s)")
    parser.add_argument("--baud", type=int, default=115200, help="Must match SERIAL_BAUD_RATE in main.c (default: %(default)s)")
    parser.add_argument("--interval", type=float, default=1.0, help="Seconds between updates; also the CPU sampling window (default: %(default)s)")
    args = parser.parse_args()

    # Warm up psutil's CPU percent counters so the first real reading isn't 0/garbage.
    psutil.cpu_percent(percpu=True)
    time.sleep(0.2)

    while True:
        try:
            with serial.Serial(args.port, args.baud, timeout=1) as ser:
                print(f"Connected to {args.port} at {args.baud} baud", file=sys.stderr)
                while True:
                    line = build_stats_line(args.interval)
                    ser.write((line + "\n").encode("ascii", errors="replace"))
                    ser.flush()
        except serial.SerialException as exc:
            print(f"Serial error ({exc}); retrying in 3s", file=sys.stderr)
            time.sleep(3)
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    main()
