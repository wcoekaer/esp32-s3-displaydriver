#!/usr/bin/env python3
"""tdisplay: drive a LilyGO T-Display-S3 running the esp32-s3-displaydriver firmware.

Command line:
    tdisplay.py ping                        firmware version, uptime
    tdisplay.py send 'TITLE RF SCANNER' 'BIG Emitters|78'   send command lines
    some-program | tdisplay.py pipe         every stdin line goes to the display
    tdisplay.py demo                        show what the display can do
    tdisplay.py shot screen.png             save a screenshot of the display
    tdisplay.py help                        the firmware's command list

Library:
    from tdisplay import TDisplay
    with TDisplay() as d:
        d.title("RF SCANNER"); d.status("ok", "LIVE")
        d.gauge("CPU", 42, unit="%"); d.log("+ scanner started")

The port is found automatically (Espressif USB VID 303a), or set --port / TDISPLAY_PORT.
Needs pyserial. Works with Python 3.6+.
"""

import argparse
import base64
import glob
import math
import os
import struct
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("tdisplay: pyserial is required (pip install pyserial, or dnf/apt install python3-pyserial)")

ESPRESSIF_VID = 0x303A
LEVELS = ("ok", "warn", "crit", "info", "off")


def find_port():
    """The display's serial port: TDISPLAY_PORT, else the first Espressif USB device."""
    if os.environ.get("TDISPLAY_PORT"):
        return os.environ["TDISPLAY_PORT"]
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    for pattern in ("/dev/tdisplay", "/dev/ttyACM*", "/dev/cu.usbmodem*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    raise SystemExit("tdisplay: no display found (connect it, or set --port / TDISPLAY_PORT)")


def _field(value):
    """Fields are '|'-separated and lines end in newline: keep both out of values."""
    return str(value if value is not None else "").replace("|", "/").replace("\n", " ").replace("\r", " ")


class TDisplay:
    """One open connection to the display. Every method sends one command line."""

    def __init__(self, port=None, timeout=2.0):
        s = serial.Serial()
        s.port = port or find_port()
        s.baudrate = 115200          # ignored by the USB CDC port, kept for adapters
        s.timeout = timeout
        # The ESP32-S3 USB serial/JTAG port resets the chip whenever RTS=1 and DTR=0.
        # The OS raises both on open; pyserial then sets DTR before RTS, so ask for
        # DTR=1, RTS=0: (1,1) -> (1,0), and on close (1,0) -> (0,0). Never (DTR 0, RTS 1).
        s.dtr = True
        s.rts = False
        s.open()
        self.serial = s

    def close(self):
        self.serial.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # --- raw ---------------------------------------------------------------
    def send(self, line):
        self.serial.write((str(line).replace("\n", " ") + "\n").encode("ascii", "replace"))
        self.serial.flush()

    def command(self, cmd, *fields):
        self.send(("%s %s" % (cmd, "|".join(_field(f) for f in fields))).rstrip())

    # --- widgets --------------------------------------------------------------
    def title(self, text):
        self.send("TITLE " + _field(text))

    def status(self, level, text):
        self.command("STATUS", level, text)

    def big(self, label, value, unit="", level="info"):
        self.command("BIG", label, value, unit, level)

    def bar(self, label, value, max_value=100, text="", level="info"):
        self.command("BAR", label, value, max_value, text, level)

    def gauge(self, label, value, lo=0, hi=100, unit="", level="info"):
        self.command("GAUGE", label, value, lo, hi, unit, level)

    def spark(self, label, values, unit="", level="info"):
        self.command("SPARK", label, ",".join("%g" % v for v in values), unit, level)

    def spark_add(self, label, value, unit="", level="info"):
        self.command("SPARK", label, "+%g" % value, unit, level)

    def kv(self, label, value, level="info"):
        self.command("KV", label, value, level)

    def led(self, label, level="ok", text=""):
        self.command("LED", label, level, text)

    def delete(self, label):
        self.send("DEL " + _field(label))

    def clear(self):
        self.send("CLEAR")

    def cls(self):
        self.send("CLS")

    def reset(self):
        self.send("RESET")

    def log(self, text):
        self.send("LOG " + _field(text))

    def alert(self, text, level="crit", seconds=10):
        self.command("ALERT", level, text, seconds)

    def alert_off(self):
        self.send("ALERT off")

    def ticker(self, text=""):
        self.send("TICKER " + _field(text))

    def page(self, name="auto"):
        self.send("PAGE " + name)

    def brightness(self, value):
        self.send("BRIGHT %d" % max(0, min(255, int(value))))

    # --- queries ---------------------------------------------------------------
    def _read_until(self, prefix, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            line = self.serial.readline().decode("ascii", "replace").strip()
            if line.startswith(prefix):
                return line
        raise TimeoutError("no %s reply from the display" % prefix)

    def ping(self):
        self.serial.reset_input_buffer()
        self.send("PING")
        return self._read_until("PONG")

    def help_text(self):
        self.serial.reset_input_buffer()
        self.send("HELP")
        time.sleep(0.4)
        return self.serial.read(self.serial.in_waiting or 1).decode("ascii", "replace")

    def screenshot(self, path, scale=1):
        """Save the current screen as a PNG (no extra libraries needed); scale enlarges it."""
        self.serial.reset_input_buffer()
        self.send("SHOT")
        head = self._read_until("SHOT ")
        w, h = (int(x) for x in head.split()[1:3])
        chunks = []
        end = time.time() + 30
        while time.time() < end:
            line = self.serial.readline().decode("ascii", "replace").strip()
            if line == "END":
                break
            chunks.append(line)
        raw = base64.b64decode("".join(chunks))
        if len(raw) != w * h * 2:
            raise ValueError("screenshot: got %d bytes, expected %d" % (len(raw), w * h * 2))
        write_png(path, w, h, raw, scale)
        return w * scale, h * scale


def write_png(path, w, h, rgb565_be, scale=1):
    """RGB565 (big-endian) -> 8-bit RGB PNG, using only the standard library.
    scale > 1 enlarges it with crisp (nearest-neighbour) pixels."""
    scale = max(1, int(scale))
    rows = bytearray()
    for y in range(h):
        line = bytearray()
        base = y * w * 2
        for x in range(w):
            v = (rgb565_be[base + 2 * x] << 8) | rgb565_be[base + 2 * x + 1]
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            line += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * scale
        for _ in range(scale):
            rows.append(0)  # filter: none
            rows += line
    w, h = w * scale, h * scale

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ------------------------------------------------------------------------- demo
def demo(d, loops=0):
    """A tour of the widgets. loops=0 runs until interrupted."""
    d.reset()
    d.title("RF SCANNER")
    d.status("ok", "LIVE")
    d.ticker("Remote ID 0/h  -  2.4 GHz: 41 emitters  -  5.8 GHz: 37 emitters  -  GPS 3D fix, 11 sats")
    series = [round(30 + 10 * math.sin(i / 3.0), 1) for i in range(40)]
    d.spark("Noise", series, " dB")
    n = 0
    while True:
        cpu = 35 + 25 * math.sin(n / 5.0) + 10 * math.sin(n / 1.7)
        d.gauge("CPU", round(cpu), unit="%", level="warn" if cpu > 60 else "ok")
        d.big("Emitters", 78 + int(6 * math.sin(n / 4.0)))
        d.bar("Disk", 63, text="63%  /var 41 GB free")
        d.spark_add("Noise", round(30 + 10 * math.sin(n / 3.0) + 2 * math.sin(n * 1.3), 1), " dB")
        d.led("HackRF", "ok", "sweeping")
        d.kv("Last drone", "none in 24 h", "ok")
        if n % 20 == 10:
            d.log("? 5.6 GHz: new wideband emitter (20 MHz)")
        if n % 40 == 25:
            d.alert("Remote ID: DJI drone 0.3 mi north", "warn", 4)
        n += 1
        if loops and n >= loops:
            break
        time.sleep(0.5)


def main(argv=None):
    p = argparse.ArgumentParser(description="Drive a T-Display-S3 USB display (esp32-s3-displaydriver).")
    p.add_argument("--port", help="serial port (default: auto, or TDISPLAY_PORT)")
    sub = p.add_subparsers(dest="cmd")
    s = sub.add_parser("send", help="send command lines")
    s.add_argument("lines", nargs="+")
    sub.add_parser("pipe", help="send every line from stdin")
    sub.add_parser("ping", help="firmware version and uptime")
    sub.add_parser("help", help="the firmware's command list")
    s = sub.add_parser("demo", help="show what the display can do")
    s.add_argument("--loops", type=int, default=0, help="stop after N updates (default: run until Ctrl-C)")
    s = sub.add_parser("shot", help="save a screenshot as PNG")
    s.add_argument("path")
    s.add_argument("--scale", type=int, default=1, help="enlarge N times (crisp pixels), e.g. 2 for docs")
    args = p.parse_args(argv)
    if not args.cmd:
        p.print_help()
        return 2
    try:
        with TDisplay(args.port) as d:
            if args.cmd == "send":
                for line in args.lines:
                    d.send(line)
            elif args.cmd == "pipe":
                for line in sys.stdin:
                    d.send(line.rstrip("\n"))
            elif args.cmd == "ping":
                print(d.ping())
            elif args.cmd == "help":
                print(d.help_text().rstrip())
            elif args.cmd == "demo":
                demo(d, args.loops)
            elif args.cmd == "shot":
                w, h = d.screenshot(args.path, args.scale)
                print("saved %s (%dx%d)" % (args.path, w, h))
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
