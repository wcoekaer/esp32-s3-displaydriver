#!/usr/bin/env python3
"""System monitor for the T-Display-S3 USB display (Linux).

Shows CPU (gauge), memory (bar), CPU temperature (big number), root disk (bar),
network throughput (sparkline) and load / uptime (key/value), refreshed every
--interval seconds. Reconnects if the display is unplugged.

    sysmon.py [--port /dev/tdisplay] [--interval 2] [--title "$(hostname)"]

Needs tdisplay.py importable: install it as /usr/local/bin/tdisplay (the default
search path below) or put it next to this file.
"""

import argparse
import glob
import os
import shutil
import socket
import sys
import time

for path in (os.path.dirname(os.path.abspath(__file__)), os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
             "/usr/local/bin", "/usr/local/lib/tdisplay"):
    sys.path.insert(0, path)
try:
    from tdisplay import TDisplay
except ImportError:
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.SourceFileLoader("tdisplay", "/usr/local/bin/tdisplay")
    spec = importlib.util.spec_from_loader("tdisplay", loader)
    tdisplay = importlib.util.module_from_spec(spec)
    loader.exec_module(tdisplay)
    TDisplay = tdisplay.TDisplay


def cpu_times():
    with open("/proc/stat") as f:
        parts = [int(x) for x in f.readline().split()[1:]]
    idle = parts[3] + (parts[4] if len(parts) > 4 else 0)
    return sum(parts), idle


def mem_percent():
    info = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":", 1)
            info[k] = int(v.split()[0])
    total, avail = info.get("MemTotal", 1), info.get("MemAvailable", info.get("MemFree", 0))
    return 100.0 * (total - avail) / total, total / 1048576.0


def cpu_temp():
    best = None
    for zone in glob.glob("/sys/class/thermal/thermal_zone*"):
        try:
            kind = open(os.path.join(zone, "type")).read().strip().lower()
            t = int(open(os.path.join(zone, "temp")).read()) / 1000.0
        except (OSError, ValueError):
            continue
        if any(k in kind for k in ("x86_pkg", "cpu", "soc", "coretemp", "k10temp")) or best is None:
            best = t
    return best


def net_bytes():
    total = 0
    with open("/proc/net/dev") as f:
        for line in f.readlines()[2:]:
            name, data = line.split(":", 1)
            if name.strip() == "lo":
                continue
            fields = data.split()
            total += int(fields[0]) + int(fields[8])
    return total


def uptime_text():
    secs = float(open("/proc/uptime").read().split()[0])
    d, rem = divmod(int(secs), 86400)
    h, rem = divmod(rem, 3600)
    return ("%dd %dh" % (d, h)) if d else ("%dh %dm" % (h, rem // 60))


def run(port, interval, title):
    with TDisplay(port) as d:
        d.reset()
        d.title(title)
        d.status("ok", "UP")
        prev_cpu, prev_net, prev_t = cpu_times(), net_bytes(), time.time()
        while True:
            time.sleep(interval)
            cpu, net, now = cpu_times(), net_bytes(), time.time()
            busy = 100.0 * (1 - (cpu[1] - prev_cpu[1]) / max(1, cpu[0] - prev_cpu[0]))
            rate = (net - prev_net) / max(0.001, now - prev_t) / 1e6 * 8  # Mbit/s
            prev_cpu, prev_net, prev_t = cpu, net, now
            mem, mem_gb = mem_percent()
            du = shutil.disk_usage("/")
            disk = 100.0 * du.used / du.total
            temp = cpu_temp()
            load = os.getloadavg()[0]
            d.gauge("CPU", round(busy), unit="%", level="crit" if busy > 90 else "warn" if busy > 70 else "ok")
            d.bar("Memory", round(mem), text="%.0f%% of %.0f GB" % (mem, mem_gb),
                  level="crit" if mem > 90 else "warn" if mem > 75 else "ok")
            if temp is not None:
                d.big("CPU temp", round(temp), "C", "crit" if temp > 85 else "warn" if temp > 70 else "ok")
            d.bar("Disk /", round(disk), text="%.0f%%  %.0f GB free" % (disk, du.free / 1e9),
                  level="crit" if disk > 95 else "warn" if disk > 85 else "ok")
            d.spark_add("Network", round(rate, 2), " Mb/s")
            d.kv("Load / uptime", "%.2f  -  %s" % (load, uptime_text()))
            d.status("warn" if busy > 70 or mem > 75 or disk > 85 else "ok", "UP %s" % uptime_text())


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--port", help="display port (default: auto / TDISPLAY_PORT)")
    p.add_argument("--interval", type=float, default=2.0)
    p.add_argument("--title", default=socket.gethostname().split(".")[0].upper())
    args = p.parse_args()
    while True:  # survive unplug/replug and reboots of the display
        try:
            run(args.port, args.interval, args.title)
        except KeyboardInterrupt:
            return 0
        except (OSError, SystemExit, Exception) as e:  # noqa: B902 - keep the service alive
            print("tdisplay-sysmon: %s; retrying in 5 s" % e, file=sys.stderr)
            time.sleep(5)


if __name__ == "__main__":
    sys.exit(main())
