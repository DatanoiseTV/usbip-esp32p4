#!/usr/bin/env python3
"""
Scan the I2C bus on an FT4232H Port A via MPSSE (pyftdi / WinUSB), over USB/IP or
local. Adapted from the usb-over-fiber repo's usbfiber/i2c_scan.py.

Setup (same as mpsse_test.py):
  - Zadig-bind Port A = Interface 0 to WinUSB (or libusbK). Ports B/C/D can stay VCP.
  - pip install pyftdi libusb-package

Wiring on Port A (ADBUS), with external pull-ups on SCL and SDA:
    AD0 = SCL
    AD1 = SDA out  (tie AD1 + AD2 together = the SDA line)
    AD2 = SDA in
Connect a real I2C device (with pull-ups) or nothing is found.

Why this is a good USB/IP link probe: the I2C waveform timing is generated
on-chip by the MPSSE, so the forwarded link does NOT distort SCL/SDA. What the
link affects is the latency and integrity of the many small USB transfers a scan
makes. A healthy link returns an identical device list every run; a marginal one
shows flapping addresses or NAK/timeout errors -> use --repeat to check.

Usage:
  python i2c_scan.py
  python i2c_scan.py --freq 100000 --repeat 20
  python i2c_scan.py --url ftdi://ftdi:4232h:<serial>/1
"""
import argparse
import sys

# Wire pyusb's libusb backend to libusb-package's bundled DLL (Windows has no
# system libusb-1.0.dll). Must run before importing pyftdi. See mpsse_test.py.
try:
    import libusb_package
    import usb.backend.libusb1 as _libusb1
    _orig_get_backend = _libusb1.get_backend
    _libusb1.get_backend = lambda find_library=None: _orig_get_backend(
        find_library=libusb_package.find_library)
except ImportError:
    pass

from pyftdi.i2c import I2cController
from pyftdi.usbtools import UsbTools, UsbToolsError

DEFAULT_URL = "ftdi://ftdi:4232h/1"   # Port A = interface 1 in pyftdi numbering

# pyftdi reads each candidate's product string during enumeration and aborts if
# it can't. On a marginal/partially-established link the string read can fail
# ("no langid") even when the device is otherwise addressable. I2C needs no
# string descriptor, so tolerate the failure and keep going.
_orig_get_string = UsbTools.get_string


def _safe_get_string(device, stridx):
    try:
        return _orig_get_string(device, stridx)
    except ValueError:
        return ""


UsbTools.get_string = staticmethod(_safe_get_string)


def _grid(found):
    lines = ["     " + " ".join(f"{c:x}" for c in range(16))]
    for row in range(8):
        cells = []
        for col in range(16):
            a = row * 16 + col
            if a < 0x03 or a > 0x77:
                cells.append("  ")
            elif a in found:
                cells.append(f"{a:02x}")
            else:
                cells.append("--")
        lines.append(f"{row*16:02x}:  " + " ".join(cells))
    return "\n".join(lines)


def _scan_once(i2c, lo, hi):
    # poll() sends START + address (write bit) and reports whether the device ACKed.
    return {a for a in range(lo, hi + 1) if i2c.poll(a, write=True, relax=True)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=DEFAULT_URL, help=f"pyftdi URL (default {DEFAULT_URL})")
    ap.add_argument("--freq", type=int, default=100_000, help="SCL Hz (default 100k)")
    ap.add_argument("--repeat", type=int, default=1, help="scan N times to check link stability")
    ap.add_argument("--lo", type=lambda x: int(x, 0), default=0x03)
    ap.add_argument("--hi", type=lambda x: int(x, 0), default=0x77)
    args = ap.parse_args(argv)

    i2c = I2cController()
    try:
        i2c.configure(args.url, frequency=args.freq)
    except (UsbToolsError, OSError) as e:
        print(f"Failed to open {args.url}: {e}", file=sys.stderr)
        print("Is Port A (interface 0) bound to WinUSB/libusbK via Zadig, and "
              "libusb-package installed?", file=sys.stderr)
        return 1

    runs = []
    errors = 0
    try:
        for i in range(args.repeat):
            try:
                found = _scan_once(i2c, args.lo, args.hi)
                runs.append(found)
                if args.repeat > 1:
                    print(f"run {i+1:>3}: {len(found)} device(s): "
                          + " ".join(f"0x{a:02x}" for a in sorted(found)), flush=True)
            except Exception as e:  # noqa: BLE001 - report any transport error
                errors += 1
                print(f"run {i+1:>3}: ERROR {type(e).__name__}: {e}", file=sys.stderr, flush=True)
    finally:
        i2c.terminate()

    if not runs:
        print("No successful scans.", file=sys.stderr)
        return 1

    union = set().union(*runs)
    stable = set.intersection(*runs)

    print(f"\nScanned at {args.freq/1000:.0f} kHz, {len(runs)} run(s), {errors} error(s)")
    print(_grid(union))
    print(f"\nDevices seen: {' '.join(f'0x{a:02x}' for a in sorted(union)) or '(none)'}")

    if args.repeat > 1:
        flapping = union - stable
        if flapping or errors:
            print("\n!! Link instability detected:")
            if flapping:
                print("   intermittent addresses: "
                      + " ".join(f"0x{a:02x}" for a in sorted(flapping)))
            if errors:
                print(f"   {errors}/{args.repeat} scans failed")
            print("   => transactions are being lost/corrupted across the link.")
        else:
            print(f"\nStable: identical {len(stable)} device(s) on every run.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
