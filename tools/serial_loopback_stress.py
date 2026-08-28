#!/usr/bin/env python3
"""
Serial loopback stress test for a USB/IP-forwarded FTDI channel.

Physically jumper TX -> RX on the target channel (a wire between the channel's
TXD and RXD pins), then run this against the port that channel presents:
COMx on Windows, /dev/ttyUSBx on Linux. It continuously writes an incrementing
byte sequence and verifies the identical sequence comes back, reporting
throughput each way and any loss/corruption under load.

This exercises sustained bidirectional bulk transfer end to end:
  host <-> (USB/IP over Ethernet) <-> ESP32-P4 <-> FT4232H <-> loopback wire

Run one instance per channel; launch all four at once to saturate the link
(the ~100 Mbit EMAC caps aggregate throughput at roughly 11 MB/s).

    python serial_loopback_stress.py --port COM5        --baud 3000000 --seconds 30
    python serial_loopback_stress.py --port /dev/ttyUSB0 --baud 3000000 --seconds 30

Requires pyserial:  pip install pyserial

Note: the integrity pattern is a single incrementing byte (period 256), so it
catches corruption and any non-256-aligned loss. For a strict loss count use a
lower baud where the link keeps up, or extend the pattern to a wider counter.
"""
import argparse
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not installed: pip install pyserial")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True,
                    help="serial port: COMx (Windows) or /dev/ttyUSBx (Linux)")
    ap.add_argument("--baud", type=int, default=3_000_000,
                    help="baud rate (FT4232H handles up to 12000000)")
    ap.add_argument("--seconds", type=float, default=30.0, help="test duration")
    ap.add_argument("--chunk", type=int, default=4096, help="write/read chunk size")
    args = ap.parse_args()

    # write_timeout so a stalled link raises instead of blocking a write forever
    ser = serial.Serial(args.port, args.baud, timeout=1, write_timeout=5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    stop = threading.Event()
    sent = {"n": 0}

    def writer():
        b = 0
        buf = bytearray(args.chunk)
        while not stop.is_set():
            for i in range(len(buf)):
                buf[i] = b & 0xFF
                b += 1
            try:
                ser.write(buf)
                sent["n"] += len(buf)
            except serial.SerialTimeoutException:
                # TX buffer full / link stalled -- back off instead of blocking
                time.sleep(0.05)
            except serial.SerialException as e:
                print(f"write error: {e}")
                stop.set()
                return

    wt = threading.Thread(target=writer, daemon=True)

    recv = 0
    errors = 0
    expected = 0
    synced = False
    t0 = time.time()
    wt.start()
    deadline = t0 + args.seconds
    next_report = t0 + 2.0
    try:
        while time.time() < deadline:
            data = ser.read(args.chunk)
            now = time.time()
            if now >= next_report:
                el = now - t0
                rate = recv / el / 1e6 if el > 0 else 0.0
                print(f"  [{el:4.0f}s] sent {sent['n']/1e6:6.2f} MB  recv {recv/1e6:6.2f} MB  "
                      f"({rate:5.2f} MB/s)  errs {errors}", flush=True)
                next_report = now + 2.0
            if not data:
                continue
            if not synced:
                expected = data[0]
                synced = True
            for byte in data:
                if byte != (expected & 0xFF):
                    errors += 1
                    expected = byte  # resync so one gap isn't counted forever
                expected = (expected + 1) & 0xFF
            recv += len(data)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        wt.join(timeout=2)
        ser.close()

    dt = time.time() - t0
    print(f"\nport={args.port} baud={args.baud} duration={dt:.1f}s")
    print(f"sent    : {sent['n']:>12,} bytes  ({sent['n'] / dt / 1e6:6.2f} MB/s)")
    print(f"received: {recv:>12,} bytes  ({recv / dt / 1e6:6.2f} MB/s)")
    print(f"byte errors/gaps: {errors}")
    if recv == 0:
        print("FAIL: nothing received - is TX jumpered to RX on this channel?")
    elif errors == 0:
        print("PASS: no corruption/loss detected")
    else:
        print("CHECK: mismatches detected (loss or corruption under load)")


if __name__ == "__main__":
    main()
