#!/usr/bin/env python3
"""Grab a screenshot from the ESP32-4848S040 over serial and save it as a PNG.

    python scripts/screenshot.py COM5 shot.png          # 240x240 preview (~1 s)
    python scripts/screenshot.py COM5 shot.png --full   # 480x480 (~5 s)

The firmware answers 'S' / 'F' with "SHOT w h" at the normal baud rate,
switches to 921600 for the raw little-endian RGB565 rows, then switches back
(see serial_screenshot_poll() in src/main.cpp). Needs pyserial, which
PlatformIO's bundled Python already has:
    ~/.platformio/penv/Scripts/python scripts/screenshot.py ...
"""
import argparse
import struct
import sys
import time
import zlib

import serial

SHOT_BAUD = 921600


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("out", help="output .png path")
    ap.add_argument("--full", action="store_true", help="full 480x480 instead of 240x240")
    ap.add_argument("--baud", type=int, default=115200, help="firmware's normal baud rate")
    args = ap.parse_args()

    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, args.baud, 0.02
    port.dtr = port.rts = False  # opening the port must not reset the board
    port.open()
    time.sleep(0.1)
    port.reset_input_buffer()
    port.write(b"F" if args.full else b"S")

    buf, start = b"", time.time()
    while True:
        buf += port.read(64)
        i = buf.find(b"SHOT ")
        if i >= 0 and b"\n" in buf[i:]:
            break
        if time.time() - start > 5:
            sys.exit("no SHOT header from the board - is the firmware running?")
    w, h = map(int, buf[i:].split(b"\n", 1)[0].split()[1:3])

    port.baudrate = SHOT_BAUD  # the board waits 300 ms before sending
    port.timeout = 1
    need, data, start = w * h * 2, bytearray(), time.time()
    while len(data) < need and time.time() - start < 60:
        data += port.read(need - len(data))
    port.baudrate = args.baud
    port.close()
    if len(data) < need:
        sys.exit(f"short read: got {len(data)} of {need} bytes")

    rgb = bytearray(w * h * 3)
    for k in range(w * h):
        v = data[2 * k] | (data[2 * k + 1] << 8)
        rgb[3 * k] = (((v >> 11) & 31) * 527 + 23) >> 6
        rgb[3 * k + 1] = (((v >> 5) & 63) * 259 + 33) >> 6
        rgb[3 * k + 2] = ((v & 31) * 527 + 23) >> 6
    write_png(args.out, w, h, bytes(rgb))
    print(f"saved {args.out} ({w}x{h})")


if __name__ == "__main__":
    main()
