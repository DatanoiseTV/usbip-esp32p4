# USB/IP Server for ESP32-P4

A standalone USB/IP server running on the ESP32-P4-Nano that exports locally-connected USB devices over Ethernet. Plug in USB devices (directly or via a hub), and they appear as native USB devices on remote machines running Linux, Windows, or macOS.

## Features

- **Wire-compatible** with standard USB/IP clients (`usbip` on Linux, `usbip-win`/`usbip-win2` on Windows)
- **USB 2.0 High-Speed** (480 Mbps) with all transfer types: control, bulk, interrupt, isochronous
- **Hub support** for multiple simultaneous devices
- **100 Mbps Ethernet** with IEEE 802.3x flow control and tuned lwIP stack
- **mDNS discovery** (`_usbip._tcp`) - auto-discoverable on the network
- **Real-time Web UI** dashboard with device tree, bandwidth monitoring, and event log
- **Access control** with open/closed mode and IP allowlist (persisted in NVS)
- **360 MHz dual-core RISC-V** with 32 MB PSRAM

## Hardware

| Component | Specification |
|-----------|--------------|
| Board | ESP32-P4-Nano |
| Ethernet PHY | IP101 (RMII) |
| USB | USB 2.0 OTG High-Speed, 16 host channels |
| Memory | 768 KB internal SRAM + 32 MB PSRAM |
| Flash | 16 MB |

## Quick Start

### Prerequisites

- [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/) with ESP32-P4 support
- ESP32-P4-Nano board with IP101 Ethernet PHY
- USB device(s) to export

### Build and Flash

```bash
source /path/to/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Connect from Linux

```bash
# Discover the server
avahi-browse _usbip._tcp -r

# List available devices
usbip list -r 192.168.1.24

# Attach a device
sudo usbip attach -r 192.168.1.24 -b 1-1

# Verify
lsusb

# Detach when done
sudo usbip detach -p 0
```

### Connect from Windows

Use [usbip-win2](https://github.com/vadimgrn/usbip-win2):

```powershell
usbip list -r 192.168.1.24
usbip attach -r 192.168.1.24 -b 1-1
```

### Web Dashboard

Open `http://192.168.1.24/` in a browser for the real-time monitoring dashboard.

The dashboard shows:
- System status (memory, uptime, device count)
- USB device tree with connection status
- Bandwidth monitoring with live chart
- Connected clients
- Event log
- Settings (network, access control, system)

## Architecture

```
ESP32-P4-Nano
+---------------------------------------------------+
|  Core 0 (high priority)    Core 1 (network)       |
|  +------------------+      +------------------+   |
|  | USB Host Daemon  |      | USB/IP Server    |   |
|  | Device Enum/Hub  |      | TCP :3240        |   |
|  | Transfer Engine  |<---->| Connection Mgr   |   |
|  +------------------+      +------------------+   |
|                            | HTTP Server :80  |   |
|                            | WebSocket + mDNS |   |
|                            +------------------+   |
+------------+-------------------+------------------+
             |                   |
        USB-A Port          RJ45 Ethernet
```

### Components

| Component | Description |
|-----------|-------------|
| `main` | Application entry, subsystem initialization |
| `usb_host_mgr` | USB host daemon, device enumeration, hub support |
| `device_manager` | Device registry with import/release lifecycle |
| `usbip_proto` | USB/IP wire protocol structures and serialization |
| `usbip_server` | TCP listener, DEVLIST and IMPORT handlers |
| `transfer_engine` | URB forwarding bridge (USB <-> network) |
| `network_mgr` | IP101 Ethernet, DHCP, mDNS service announcement |
| `webui` | HTTP server, WebSocket, HTMX dashboard |
| `access_control` | Open/closed mode, IP allowlist, NVS persistence |
| `event_log` | PSRAM ring buffer for structured event logging |

## USB/IP Protocol

Implements USB/IP protocol versions:
- **v1.0.0** (`0x0100`) - original protocol
- **v1.1.1** (`0x0111`) - current Linux kernel 6.x, Windows clients

The server auto-detects the client's protocol version and echoes it back, ensuring compatibility with all known USB/IP clients.

### Supported Operations

| Operation | Description |
|-----------|-------------|
| `OP_REQ_DEVLIST` | List all exported USB devices |
| `OP_REP_DEVLIST` | Device list response with descriptors |
| `OP_REQ_IMPORT` | Attach (claim) a specific device |
| `OP_REP_IMPORT` | Import response with device descriptor |
| `USBIP_CMD_SUBMIT` | Submit a USB transfer (URB) |
| `USBIP_RET_SUBMIT` | Transfer completion result |
| `USBIP_CMD_UNLINK` | Cancel a pending transfer |
| `USBIP_RET_UNLINK` | Unlink result |

## Configuration

### sdkconfig Defaults

Key settings in `sdkconfig.defaults`:

| Setting | Value | Purpose |
|---------|-------|---------|
| CPU frequency | 360 MHz | Maximum performance |
| PSRAM | 32 MB, HEX mode, 200 MHz | Large buffer storage |
| Internal DMA reserve | 256 KB | USB + Ethernet DMA buffers |
| TCP send/recv window | 32 KB | High throughput |
| TCP MSS | 1460 | Maximum for Ethernet |
| lwIP RTO | 300 ms | Fast retransmit on LAN |
| USB hub support | Multi-level | Hub + direct devices |
| Ethernet DMA | 16 RX + 16 TX descriptors | Burst absorption |
| Watchdog | 10 s | System health monitoring |

### Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `USBIP_TCP_PORT` | 3240 | USB/IP server TCP port |
| `USBIP_MAX_DEVICES` | 8 | Maximum exportable USB devices |
| `USBIP_MAX_CLIENTS` | 4 | Maximum concurrent client connections |
| `USBIP_HOSTNAME` | `usbip-esp32p4` | mDNS hostname |

### Ethernet Pin Map (ESP32-P4-Nano + IP101)

| Signal | GPIO |
|--------|------|
| TXD0 | 34 |
| TXD1 | 35 |
| TX_EN | 49 |
| RXD0 | 29 |
| RXD1 | 30 |
| CRS_DV | 28 |
| REF_CLK | 50 |
| MDC | 31 |
| MDIO | 52 |
| PHY_RST | 51 |

## Access Control

By default, the server runs in **open mode** - any machine on the network can attach to exported devices.

To restrict access:
1. Open the Web UI at `http://<ip>/settings`
2. Enable "Closed mode"
3. Add allowed IP addresses to the allowlist

Settings are persisted in NVS and survive reboots.

## Use Cases

- **Remote USB dongles** - share USB license dongles, security keys (FIDO2/U2F), or hardware tokens across the network without physical access
- **IC/MCU programmers** - flash and debug embedded devices remotely (JTAG/SWD adapters, UART bridges, ISP programmers)
- **Lab equipment** - share USB test instruments (oscilloscopes, logic analyzers, multimeters) between workstations
- **USB peripherals** - use USB barcode scanners, card readers, or specialized input devices from any machine on the network
- **Build servers** - attach USB devices to CI/CD runners for hardware-in-the-loop testing
- **Thin clients** - redirect USB storage, printers, or other peripherals to virtual machines or remote desktops
- **Headless servers** - access USB devices plugged into rack-mounted ESP32-P4 units without physical console access

## Performance

The effective throughput is limited by the **100 Mbps Ethernet** link and the ESP32-P4's processing overhead for USB/IP protocol conversion. While the USB 2.0 HS bus supports 480 Mbps, the achievable USB/IP throughput is:

| Bottleneck | Limit |
|------------|-------|
| Ethernet wire speed | ~12 MB/s (100 Mbps) |
| TCP/IP overhead | ~11 MB/s effective |
| USB/IP protocol overhead | ~10 MB/s for bulk transfers |
| Practical throughput | **5-10 MB/s** depending on transfer pattern |

This is more than sufficient for most USB devices. IC programmers, HID devices, dongles, serial adapters, and even many storage operations work well within these limits. Devices that require sustained high-bandwidth (e.g., USB 2.0 webcams at high resolution) may experience reduced frame rates.

**Latency** is typically 1-3 ms per USB transaction over a local Ethernet network.

## Known Limitations

- **USB 2.0 only** - no USB 3.x SuperSpeed (hardware limitation)
- **Ethernet only** - no WiFi transport (by design, for reliability and latency)
- **No Transaction Translator** - FS/LS devices only work when connected directly to the root port, not through HS hubs (ESP-IDF limitation)
- **100 Mbps Ethernet** - throughput limited by network, not USB bus speed (see Performance section)
- **ESP-IDF USB host stack** - some devices with multiple configurations may have enumeration issues

## License

This project builds on the [USB/IP protocol](https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html) originally developed by Takahiro Hirofuchi.
