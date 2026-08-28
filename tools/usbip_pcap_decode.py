#!/usr/bin/env python3
"""
Decode a USB/IP capture written by the usbip-esp32p4 `capture` component.

That component records traffic as a libpcap file with linktype 220
(LINKTYPE_USB_LINUX_MMAPPED), i.e. each record is a 64-byte Linux usbmon
binary header followed by up to snaplen bytes of URB data. Wireshark opens
these natively; this script gives a scriptable, human-readable transaction
trace and flags the things that matter for debugging: URB errors and submits
that never completed (hung/dropped transfers).

    python tools/usbip_pcap_decode.py cap_0000.pcap
    python tools/usbip_pcap_decode.py cap_0000.pcap --hex 32     # + payload hexdump
    python tools/usbip_pcap_decode.py cap_0000.pcap --errors-only
"""
import argparse
import struct
import sys

PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_USB_LINUX_MMAPPED = 220

GLOBAL_HDR = struct.Struct("<IHHiIII")          # 24 bytes
REC_HDR = struct.Struct("<IIII")                # 16 bytes
MON_HDR = struct.Struct("<QBBBBHBBqiiII8siiII")  # 64 bytes

XFER = {0: "ISO", 1: "INT", 2: "CTRL", 3: "BULK"}
ERRNO = {0: "OK", -115: "EINPROGRESS", -32: "EPIPE(stall)", -110: "ETIMEDOUT",
         -71: "EPROTO", -75: "EOVERFLOW", -121: "EREMOTEIO", -2: "ENOENT",
         -108: "ESHUTDOWN", -19: "ENODEV"}


def status_str(s):
    return ERRNO.get(s, str(s))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", help="capture file (e.g. cap_0000.pcap)")
    ap.add_argument("--hex", type=int, default=0, metavar="N",
                    help="hexdump first N bytes of each URB payload")
    ap.add_argument("--errors-only", action="store_true",
                    help="only show completions with a non-OK status")
    ap.add_argument("--limit", type=int, default=0, help="stop after N records")
    args = ap.parse_args()

    with open(args.pcap, "rb") as f:
        data = f.read()

    if len(data) < GLOBAL_HDR.size:
        sys.exit(f"{args.pcap}: too small ({len(data)} bytes) — no packets "
                 f"(a 24-byte file is just the pcap header, zero records captured).")

    magic, vmaj, vmin, _tz, _sig, snaplen, net = GLOBAL_HDR.unpack_from(data, 0)
    if magic != PCAP_MAGIC:
        sys.exit(f"bad magic 0x{magic:08x} (expected 0x{PCAP_MAGIC:08x}) — not this pcap format")
    if net != LINKTYPE_USB_LINUX_MMAPPED:
        print(f"warning: linktype {net} (expected {LINKTYPE_USB_LINUX_MMAPPED}); decoding anyway",
              file=sys.stderr)

    print(f"{args.pcap}: pcap v{vmaj}.{vmin}, linktype={net}, snaplen={snaplen}")
    print(f"{'#':>4} {'t(s)':>10} {'ev':>2} {'xfer':>4} {'ep':>5} {'dev':>3} "
          f"{'status':>14} {'urb_len':>8} {'data':>6}")

    off = GLOBAL_HDR.size
    idx = 0
    t0 = None
    submits = {}            # id -> record index (open submits)
    n_by_type = {}
    n_err = 0
    n_records = 0
    while off + REC_HDR.size <= len(data):
        ts_sec, ts_usec, incl_len, _orig = REC_HDR.unpack_from(data, off)
        off += REC_HDR.size
        pkt = data[off:off + incl_len]
        off += incl_len
        if len(pkt) < MON_HDR.size:
            print(f"  (truncated record {idx}: {len(pkt)} bytes)")
            break
        (uid, mtype, xfer_type, ep_addr, devnum, busnum, _fs, _fd,
         mts_sec, mts_usec, status, urb_len, data_len, _setup,
         _interval, _sframe, _flags, _ndesc) = MON_HDR.unpack_from(pkt, 0)
        n_records += 1

        ev = chr(mtype) if mtype in (0x53, 0x43, 0x45) else "?"
        if ev == "S" or ev == "E":
            submits[uid] = idx
        elif ev == "C":
            submits.pop(uid, None)

        t = ts_sec + ts_usec / 1e6
        if t0 is None:
            t0 = t
        ep = f"{ep_addr & 0x7f}{'IN' if ep_addr & 0x80 else 'OUT'}"
        n_by_type[ev] = n_by_type.get(ev, 0) + 1
        is_err = (ev == "C" and status not in (0,))
        if is_err:
            n_err += 1

        if not args.errors_only or is_err:
            print(f"{idx:>4} {t - t0:>10.6f} {ev:>2} {XFER.get(xfer_type, '?'):>4} "
                  f"{ep:>5} {devnum:>3} {status_str(status):>14} {urb_len:>8} {data_len:>6}")
            if args.hex and not args.errors_only:
                payload = pkt[MON_HDR.size:MON_HDR.size + min(data_len, args.hex)]
                if payload:
                    print("        " + " ".join(f"{b:02x}" for b in payload))

        idx += 1
        if args.limit and idx >= args.limit:
            break

    print(f"\n{n_records} records: " +
          ", ".join(f"{k}={v}" for k, v in sorted(n_by_type.items())))
    print(f"completions with errors: {n_err}")
    if submits:
        print(f"submits with NO completion: {len(submits)}  "
              f"(hung/dropped URBs — record #s: {sorted(submits.values())[:20]})")
    else:
        print("submits with no completion: 0 (every URB completed)")


if __name__ == "__main__":
    main()
