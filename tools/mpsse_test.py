#!/usr/bin/env python3
"""
MPSSE self-test for FT4232H Port A (over USB/IP or local).

Port A of the FT4232H supports MPSSE (JTAG/SPI/I2C/bit-bang), which needs a
libusb-class driver -- NOT the VCP COM-port driver. First use Zadig to bind
*Interface 0* (Port A) to WinUSB (or libusbK); ports B/C/D can stay VCP.

Over USB/IP this exercises the SET_BITMODE control transfer (enter MPSSE) plus
bulk OUT (commands) / bulk IN (responses) -- tighter timing than a VCP loopback,
so it's a good forwarding-integrity test.

  pip install pyftdi libusb-package
  python mpsse_test.py                    # auto (ftdi://ftdi:4232h/1)
  python mpsse_test.py ftdi://ftdi:4232h/1

Needs a libusb backend: `libusb-package` bundles one, or put libusb-1.0.dll on PATH.
"""
import sys

# Wire pyusb's libusb backend to the DLL bundled with libusb-package. On Windows
# there's usually no system-wide libusb-1.0.dll, so pyusb's default discovery
# finds no backend and pyftdi reports an empty device list -- even with
# libusb-package installed. (Same fix as the usb-over-fiber repo's _backend.py.)
# Must run BEFORE importing pyftdi / the first usb.core.find.
try:
    import libusb_package
    import usb.backend.libusb1 as _libusb1
    _orig_get_backend = _libusb1.get_backend
    _libusb1.get_backend = lambda find_library=None: _orig_get_backend(
        find_library=libusb_package.find_library)
except ImportError:
    pass  # no libusb-package; fall back to a system libusb-1.0.dll if present

from pyftdi.ftdi import Ftdi

DEFAULT_URL = "ftdi://ftdi:4232h/1"   # FT4232H Port A = interface 1 in pyftdi's numbering


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL

    print("Detected FTDI devices (use one of these URLs if the default fails):")
    try:
        Ftdi.show_devices()
    except Exception as e:
        print(f"  device scan failed: {e}")

    print(f"\nOpening MPSSE on {url} ...")
    ftdi = Ftdi()
    # open_mpsse_from_url() issues SET_BITMODE to enter MPSSE and runs the
    # 0xAA/0xAB bad-command sync internally. If it returns, MPSSE is alive and
    # the control + bulk path over USB/IP works.
    ftdi.open_mpsse_from_url(url, frequency=1e6)
    print("  MPSSE entered OK (SET_BITMODE + internal sync passed)")

    # --- Explicit bad-command sync: invalid opcode -> chip echoes 0xFA <op> ---
    ftdi.write_data(bytes([0xAB, 0x87]))          # 0xAB invalid, 0x87 = send-immediate
    resp = bytes(ftdi.read_data_bytes(2, attempt=4))
    ok_sync = resp == bytes([0xFA, 0xAB])
    print(f"  bad-command sync: sent 0xAB -> got {resp.hex(' ') or '(none)'} "
          f"[{'PASS' if ok_sync else 'FAIL'}]")

    # --- Internal loopback data echo: DO tied to DI inside the chip (no wiring) ---
    ftdi.write_data(bytes([0x84]))                # loopback ON
    payload = bytes(range(64))
    n = len(payload) - 1
    ftdi.write_data(bytes([0x31, n & 0xFF, (n >> 8) & 0xFF]) + payload)  # clock out+in, MSB
    ftdi.write_data(bytes([0x87]))                # send immediate
    echoed = bytes(ftdi.read_data_bytes(len(payload), attempt=4))
    ftdi.write_data(bytes([0x85]))                # loopback OFF
    ok_loop = echoed == payload
    print(f"  MPSSE loopback: {len(echoed)}/{len(payload)} bytes echoed "
          f"[{'PASS' if ok_loop else 'MISMATCH (edge/opcode, or forwarding corruption)'}]")

    ftdi.close()

    print("\nRESULT:",
          "MPSSE control+bulk works over this link"
          if ok_sync else "MPSSE FAILED -- control/bulk not round-tripping (see above)")
    if ok_sync and not ok_loop:
        print("  (sync passed but data echo mismatched -- compare against the other firmware"
              " before blaming a firmware; could be the clock opcode/edges.)")


if __name__ == "__main__":
    main()
