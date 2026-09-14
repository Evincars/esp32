#!/usr/bin/env python3
"""Streams CPU/RAM/GPU/network stats to the ESP32 TFT monitor over a serial port.

Reads per-core CPU usage, CPU temperature, RAM usage, NVIDIA GPU/VRAM usage
and temperature, network upload/download rates, and process/thread counts,
then sends one line per sample to the ESP32 (main.c), which renders it as
graphs. Any other text sent to the same serial port (e.g. "ping" output) is
rendered by the ESP32 as a plain scrolling log instead, so this script does
not need to be the only thing writing to the port.

Wire format (single ASCII line terminated by "\\n"):

    #SYS#cpus=<c0>;<c1>;...|cputemp=<c>|ramused=<mb>|ramtotal=<mb>|
         vramused=<mb>|vramtotal=<mb>|gpuusage=<pct>|gputemp=<c>|
         netup=<kBps>|netdown=<kBps>|procs=<n>|threads=<n>|
         corethreads=<t0>;<t1>;...#END#

A metric that can't be read is sent as an empty value (e.g. "gputemp="); the
ESP32 side shows "N/A" for it. GPU stats and per-core thread counts are
frequently unavailable depending on the machine, and that is expected.

Requirements: pip install psutil pyserial
GPU stats require NVIDIA's `nvidia-smi` on PATH; other vendors are not
supported yet and will simply show up as N/A. Per-core thread counts are
Linux-only (read from /proc) and are omitted elsewhere.
"""

import argparse
import glob
import subprocess
import sys
import time
from typing import List, Optional, Tuple

import psutil
import serial

STATS_PREFIX = "#SYS#"
STATS_SUFFIX = "#END#"
IS_LINUX = sys.platform.startswith("linux")


def read_cpu_usage_percpu(interval: float) -> List[float]:
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


def read_process_count() -> int:
    return len(psutil.pids())


def read_thread_stats(core_count: int) -> Tuple[Optional[int], Optional[List[int]]]:
    """Best-effort Linux-only per-core thread count, from each thread's last-run CPU.

    Returns (total_threads, per_core_counts) or (None, None) when unavailable.
    """
    if not IS_LINUX or core_count <= 0:
        return None, None

    per_core = [0] * core_count
    total = 0
    for stat_path in glob.glob("/proc/[0-9]*/task/[0-9]*/stat"):
        try:
            with open(stat_path, "r") as f:
                content = f.read()
        except OSError:
            continue
        try:
            # comm (field 2) may contain spaces/parens, so split after its closing ')'.
            after_comm = content.rsplit(")", 1)[1]
            fields = after_comm.split()
            processor = int(fields[36])  # field 39 ("processor"), offset by fields 3.. starting at index 0
        except (IndexError, ValueError):
            continue
        total += 1
        if 0 <= processor < core_count:
            per_core[processor] += 1
    return total, per_core


class NetworkRateTracker:
    """Computes upload/download KB/s from successive psutil.net_io_counters() samples."""

    def __init__(self) -> None:
        self._prev_counters = None
        self._prev_time: Optional[float] = None

    def sample(self) -> Tuple[Optional[float], Optional[float]]:
        counters = psutil.net_io_counters()
        now = time.monotonic()
        upload_kbps = None
        download_kbps = None
        if self._prev_counters is not None and self._prev_time is not None:
            elapsed = now - self._prev_time
            if elapsed > 0:
                sent_delta = counters.bytes_sent - self._prev_counters.bytes_sent
                recv_delta = counters.bytes_recv - self._prev_counters.bytes_recv
                upload_kbps = max(0.0, sent_delta / elapsed / 1024.0)
                download_kbps = max(0.0, recv_delta / elapsed / 1024.0)
        self._prev_counters = counters
        self._prev_time = now
        return upload_kbps, download_kbps


def format_value(value: Optional[float], decimals: int = 1) -> str:
    return "" if value is None else f"{value:.{decimals}f}"


def build_stats_line(interval: float, net_tracker: NetworkRateTracker) -> str:
    cpu_usage = read_cpu_usage_percpu(interval)
    cpu_temp = read_cpu_temperature()
    ram_used_mb, ram_total_mb = read_ram_usage()
    vram_used, vram_total, gpu_usage, gpu_temp = read_nvidia_gpu()
    net_up_kbps, net_down_kbps = net_tracker.sample()
    process_count = read_process_count()
    thread_total, threads_per_core = read_thread_stats(len(cpu_usage))

    fields = [
        "cpus=" + ";".join(f"{v:.1f}" for v in cpu_usage),
        "cputemp=" + format_value(cpu_temp),
        "ramused=" + format_value(ram_used_mb, 0),
        "ramtotal=" + format_value(ram_total_mb, 0),
        "vramused=" + format_value(vram_used, 0),
        "vramtotal=" + format_value(vram_total, 0),
        "gpuusage=" + format_value(gpu_usage, 0),
        "gputemp=" + format_value(gpu_temp),
        "netup=" + format_value(net_up_kbps, 1),
        "netdown=" + format_value(net_down_kbps, 1),
        "procs=" + str(process_count),
        "threads=" + ("" if thread_total is None else str(thread_total)),
        "corethreads=" + ("" if threads_per_core is None else ";".join(str(t) for t in threads_per_core)),
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
    net_tracker = NetworkRateTracker()

    while True:
        try:
            with serial.Serial(args.port, args.baud, timeout=1) as ser:
                print(f"Connected to {args.port} at {args.baud} baud", file=sys.stderr)
                while True:
                    line = build_stats_line(args.interval, net_tracker)
                    ser.write((line + "\n").encode("ascii", errors="replace"))
                    ser.flush()
        except serial.SerialException as exc:
            print(f"Serial error ({exc}); retrying in 3s", file=sys.stderr)
            time.sleep(3)
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    main()
