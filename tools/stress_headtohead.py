#!/usr/bin/env python3
"""
Head-to-head stress runner: drive N FT4232H channels concurrently through one
firmware, aggregate throughput, and append a labeled row to a CSV so two
firmwares can be compared apples-to-apples.

It launches serial_loopback_stress.py (same directory) once per port, in
parallel, parses each result, and records the aggregate. Flash firmware A, run
with --label A; reflash firmware B, run the SAME command with --label B; then
diff the CSV.

    # 4 channels, 60 s, 12 Mbaud, tagged for the DatanoiseTV firmware
    python tools/stress_headtohead.py --label usbip-esp32p4 \
        --ports /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyUSB2 /dev/ttyUSB3 \
        --baud 12000000 --seconds 60

    # after reflashing the other firmware, identical run:
    python tools/stress_headtohead.py --label usbipdcpp_esp32 \
        --ports /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyUSB2 /dev/ttyUSB3 \
        --baud 12000000 --seconds 60

Windows: use COM ports, e.g. --ports COM5 COM6 COM7 COM8

Keep every variable identical between the two runs — only the firmware (and
--label) should change. Requires pyserial (used by the underlying script).
"""
import argparse
import csv
import os
import re
import subprocess
import sys
from datetime import datetime

RECV_RE = re.compile(r"received:\s*[\d,]+\s*bytes\s*\(\s*([\d.]+)\s*MB/s\)")
ERR_RE = re.compile(r"byte errors/gaps:\s*(\d+)")
VERDICT_RE = re.compile(r"^(PASS|CHECK|FAIL)", re.MULTILINE)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, help="firmware name for this run (e.g. usbip-esp32p4)")
    ap.add_argument("--ports", nargs="+", required=True, help="serial ports, one per channel")
    ap.add_argument("--baud", type=int, default=3_000_000)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--csv", default="stress_results.csv", help="CSV to append the aggregate row to")
    ap.add_argument("--script", default=None, help="path to serial_loopback_stress.py (default: alongside this file)")
    args = ap.parse_args()

    script = args.script or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "serial_loopback_stress.py")
    if not os.path.exists(script):
        sys.exit(f"cannot find serial_loopback_stress.py at {script} (use --script)")

    print(f"[{args.label}] {len(args.ports)} channel(s) @ {args.baud} baud for {args.seconds:g}s\n")

    # Launch one stress process per port, concurrently.
    procs = []
    for port in args.ports:
        cmd = [sys.executable, script, "--port", port,
               "--baud", str(args.baud), "--seconds", str(args.seconds)]
        procs.append((port, subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                              stderr=subprocess.STDOUT, text=True)))

    per_channel = []
    total_mbps = 0.0
    total_errors = 0
    worst = "PASS"
    rank = {"PASS": 0, "CHECK": 1, "FAIL": 2}
    for port, p in procs:
        try:
            out, _ = p.communicate(timeout=args.seconds + 20)
        except subprocess.TimeoutExpired:
            p.kill()
            out, _ = p.communicate()
            print(f"  {port:16} TIMED OUT (killed) -- child never finished", flush=True)
        mbps = float(RECV_RE.search(out).group(1)) if RECV_RE.search(out) else 0.0
        errs = int(ERR_RE.search(out).group(1)) if ERR_RE.search(out) else -1
        verdict = VERDICT_RE.search(out).group(1) if VERDICT_RE.search(out) else "FAIL"
        per_channel.append((port, mbps, errs, verdict))
        total_mbps += mbps
        total_errors += max(errs, 0)
        if rank[verdict] > rank[worst]:
            worst = verdict
        print(f"  {port:16} {mbps:6.2f} MB/s   errors={errs:<6} {verdict}")

    print(f"\n  AGGREGATE: {total_mbps:.2f} MB/s across {len(args.ports)} channel(s), "
          f"total errors={total_errors}, verdict={worst}")

    # Append aggregate row to CSV (write header if new).
    new_file = not os.path.exists(args.csv)
    with open(args.csv, "a", newline="") as f:
        w = csv.writer(f)
        if new_file:
            w.writerow(["timestamp", "label", "baud", "seconds", "channels",
                        "aggregate_MBps", "total_errors", "verdict", "per_channel_MBps"])
        w.writerow([datetime.now().isoformat(timespec="seconds"), args.label, args.baud,
                    args.seconds, len(args.ports), f"{total_mbps:.2f}", total_errors, worst,
                    ";".join(f"{m:.2f}" for _, m, _, _ in per_channel)])
    print(f"  logged to {args.csv}")


if __name__ == "__main__":
    main()
