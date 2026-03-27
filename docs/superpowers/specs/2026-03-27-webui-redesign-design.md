# USB/IP Server WebUI Redesign + Protocol Cleanup - Design Specification

**Date:** 2026-03-27
**Target:** ESP32-P4-Nano USB/IP Server
**Scope:** Complete WebUI overhaul + drop v1.0.0 protocol + device reset + WebUI auth

---

## 1. Overview

Complete redesign of the USB/IP server Web UI from a basic dark-theme dashboard to a professional, UniFi-style light-theme management interface with live USB topology visualization, detailed device information, and full settings management. Also includes dropping USB/IP protocol v1.0.0 support (v1.1.1 only) and adding optional WebUI password protection.

---

## 2. Layout & Navigation

Single-page application with fixed left sidebar and scrollable main content area.

### Sidebar (fixed, 220px wide)
- **Logo area:** "USB/IP" bold + "ESP32-P4" subdued text
- **Connection indicator:** Green/red dot showing WebSocket status
- **Nav items:** Dashboard, Devices, Topology, Settings
- **Bottom:** System uptime, firmware version

### Main Content
Sections switched via sidebar nav clicks. Pure DOM show/hide, no URL routing, no page reloads. Single HTML file.

| Section | Purpose |
|---------|---------|
| Dashboard | Aggregate stats, bandwidth chart, recent log |
| Devices | Device list with expandable detail panels |
| Topology | Live SVG tree of USB bus |
| Settings | Network, access control, auth, system |

### Responsive Behavior
- Below 768px: sidebar collapses to hamburger menu
- Below 480px: stat cards stack vertically

---

## 3. Dashboard Section

### Stats Row (5 cards, horizontal)

| Card | Content | Update Rate |
|------|---------|-------------|
| Devices | Connected USB device count | Real-time |
| Clients | Active USB/IP client count | Real-time |
| Throughput | Aggregate IN+OUT combined rate | 500ms |
| Memory | Free internal SRAM with bar indicator | 1s |
| Uptime | Days/hours/minutes since boot | 1s |

### Aggregate Bandwidth Chart
- Pure canvas JS (no library)
- 120-point rolling window (60 seconds at 500ms intervals)
- Two lines: IN (blue) and OUT (green) with subtle filled area
- Light grid, auto-scaling Y-axis with human-readable labels (KB/s, MB/s)

### Recent Events
- Last 15 log entries, newest at top
- Timestamp + message in monospace
- Simple list, no filtering

---

## 4. Devices Section

### Collapsed View (default, one row per device)

```
[status dot] 1-1  a466:0a53  FS  Exported  12.4 KB/s  [Details v]
```

- Status dot: green=imported, blue=exported/available, gray=blocked
- busid (monospace), VID:PID, speed badge (HS/FS/LS), status text, live throughput
- If imported: client IP shown inline
- Empty state: centered "No devices connected"

### Expanded Detail Panel (click Details)

#### Device Info
- Manufacturer string
- Product string
- Serial number
- bcdDevice (firmware version)
- Device class / subclass / protocol (decoded name)

#### Configuration
- bConfigurationValue
- bNumInterfaces
- Max power (mA)

#### Interfaces Table
| # | Class | Subclass | Protocol | Endpoints |
|---|-------|----------|----------|-----------|

Class column shows decoded human-readable name from full USB class lookup table.

#### Endpoints Table (per interface)
| Address | Direction | Type | Max Packet Size | Interval |
|---------|-----------|------|-----------------|----------|

Type decoded: Control, Bulk, Interrupt, Isochronous.

#### Transfer Statistics
- URBs completed / failed
- Bytes IN / OUT (total)
- Error count
- Last activity timestamp

#### Per-Endpoint Throughput
- Numeric throughput per active endpoint

### Device Actions
- **Export / Block** toggle button
- **Disconnect Client** button (if imported, forcibly releases)
- **Reset Device** button (USB port reset, re-enumerates without unplug)

### USB Class Code Lookup Table (JavaScript)

Full standard USB class decode:

| Code | Name |
|------|------|
| 0x00 | Composite |
| 0x01 | Audio |
| 0x02 | CDC Communications |
| 0x03 | HID |
| 0x05 | Physical |
| 0x06 | Image |
| 0x07 | Printer |
| 0x08 | Mass Storage |
| 0x09 | Hub |
| 0x0A | CDC Data |
| 0x0B | Smart Card |
| 0x0D | Content Security |
| 0x0E | Video |
| 0x0F | Personal Healthcare |
| 0x10 | Audio/Video |
| 0x11 | Billboard |
| 0x12 | USB Type-C Bridge |
| 0xDC | Diagnostic |
| 0xE0 | Wireless Controller |
| 0xEF | Miscellaneous |
| 0xFE | Application Specific |
| 0xFF | Vendor Specific |

---

## 5. Topology Section

### Live SVG Tree Visualization

Auto-generated from the device registry. No external SVG library - hand-drawn SVG paths.

### Tree Structure

```
    [ESP32-P4 Host Controller]
              |
         [Root Port]
              |
         [Hub 1-1]
         4-port USB 2.0
     ┌───┬───┬───┬───┐
     P1  P2  P3  P4
     |   |
  [1-1.1] [1-1.2]
  a466:0a53  2752:0019
  IC Prog    Audio
```

For single device (no hub):
```
    [ESP32-P4 Host Controller]
              |
         [Root Port]
              |
          [1-1]
        a466:0a53
        IC Programmer
```

### Visual Design
- Nodes: rounded rectangles with busid, VID:PID, device name, speed badge
- Hub nodes: show all physical ports as small circles along bottom edge
- Empty hub ports: gray dots with port number
- Connected devices hang below their specific port
- Color-coded borders: green=imported, blue=exported, gray=empty/blocked
- Right-angle connecting lines (not diagonal)
- Host controller at top, devices below
- Click device node to jump to its detail panel in Devices section

### Data Source
- Same WebSocket stats feed
- Topology derived from busid naming convention: `1-1` = root port, `1-1.3` = hub port 3
- Tree rebuilt on every device change event

---

## 6. Settings Section

Four subsections stacked vertically.

### Network
- **Hostname** - text input, current value pre-filled
- **DHCP** - toggle checkbox
- **Static IP fields** (shown only when DHCP off): IP, Subnet Mask, Gateway, DNS
- **Current IP** - read-only info line
- **Save** button - POST to `/api/settings/network`, stores in NVS
- Reboot notification after save

### Access Control

#### WebUI Authentication
- **Enable authentication** toggle (disabled by default)
- When enabled: username + password fields (min 4 chars each)
- Change password button (shown only when auth enabled)

#### USB/IP Access
- **Mode:** Open / Closed radio buttons
- **Allowlist** (shown only in closed mode): list of IPs with remove buttons + add input

### System
- Firmware version, board info, chip revision (read-only)
- Free heap / internal heap (read-only, live-updating)
- **Restart** button (with browser confirm dialog)
- **Factory Reset** button (erases NVS, with browser confirm dialog)

---

## 7. WebUI Authentication

### Implementation
- HTTP Basic Auth over existing `esp_http_server`
- Credentials stored in NVS namespace `"auth"`:
  - `enabled` (uint8_t): 0=disabled, 1=enabled
  - `username` (string, max 32 chars)
  - `password_hash` (32 bytes, SHA-256 of password)
- When enabled: ALL HTTP and WebSocket requests require valid Basic Auth header
- When disabled: no auth required (default)
- Browser shows native login dialog (no custom login page)
- Failed auth returns HTTP 401 with `WWW-Authenticate: Basic realm="USB/IP Server"`

### Password Validation
- SHA-256 hash comparison (not plaintext storage)
- Use `mbedtls_sha256()` from ESP-IDF's bundled mbedTLS

---

## 8. Backend Changes

### Device Manager Extensions
- Store full config descriptor blob (raw bytes, up to 512 bytes, in PSRAM)
- Store string descriptors: manufacturer, product, serial (read during enumeration via control transfers)
- Store per-endpoint transfer counters

### New API Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/devices/{index}` | GET | Detailed device info (descriptors, endpoints, stats) |
| `/api/devices/{index}/reset` | POST | Trigger USB port reset |
| `/api/devices/{index}/disconnect` | POST | Force-disconnect client |
| `/api/devices/{index}/export` | POST | Toggle export/block |
| `/api/settings/network` | GET/POST | Network configuration |
| `/api/settings/auth` | GET/POST | WebUI auth settings |
| `/api/settings/acl` | GET/POST | Access control settings |
| `/api/system/restart` | POST | Reboot device |
| `/api/system/factory-reset` | POST | Erase NVS + reboot |

### USB Host Manager Extensions
- `usb_host_mgr_reset_device(uint8_t dev_addr)` - trigger USB port reset
- Read string descriptors (manufacturer/product/serial) during enumeration
- Store raw config descriptor in device_manager

### WebSocket Stats Extensions
- Per-device endpoint list with throughput
- Hub port mapping info for topology
- Per-device string descriptors (manufacturer, product)

### Protocol Change
- Remove `USBIP_VERSION_V100` (0x0100) support
- `usbip_version_supported()` only accepts `USBIP_VERSION_V111` (0x0111)
- Remove `USBIP_VERSION_V100` constant

---

## 9. Visual Theme

UniFi-style light theme. Clean, polished, spacious.

### CSS Variables
```css
--bg: #f5f6fa;
--surface: #ffffff;
--border: #e2e8f0;
--border-light: #f0f0f5;
--text: #1a202c;
--text-secondary: #64748b;
--text-dim: #94a3b8;
--accent: #3b82f6;
--accent-hover: #2563eb;
--green: #22c55e;
--red: #ef4444;
--orange: #f59e0b;
--blue: #3b82f6;
--sidebar-bg: #1e293b;
--sidebar-text: #e2e8f0;
--sidebar-active: #3b82f6;
```

### Typography
- Font: `system-ui, -apple-system, 'Segoe UI', sans-serif`
- Monospace: `'SF Mono', 'Cascadia Code', 'Consolas', monospace`
- Large stat numbers: 600 weight, tabular-nums
- Body: 400 weight, 14px base

### Cards
- White background, 1px border, 8px border-radius
- Subtle `box-shadow: 0 1px 3px rgba(0,0,0,0.08)`
- Hover: elevated shadow `0 4px 12px rgba(0,0,0,0.1)`

### Sidebar
- Dark (`#1e293b`) to contrast with light main content
- White text, active item highlighted with accent color
- Fixed position, full height

---

## 10. File Structure

### Frontend (embedded in flash)
```
components/webui/frontend/
  index.html      # Single-page app with all sections
  style.css       # Full stylesheet
  app.js          # All JavaScript (WebSocket, rendering, topology, charts)
```

Separate `app.js` file for cleaner organization. All three embedded via `EMBED_FILES`.

### Backend
```
components/webui/
  webui.c          # HTTP server, routes, auth middleware
  ws_handler.c     # WebSocket handler
  webui_api.c      # REST API endpoint handlers (new file)
```

### Flash Budget
- HTML: ~10 KB
- CSS: ~6 KB
- JS: ~15 KB
- Total: ~31 KB

---

## 11. lwIP Tuning for USB/IP Frame Sizes

USB/IP messages can be large: a CMD_SUBMIT with 16KB transfer buffer = 48-byte header + 16384-byte payload = ~16.4 KB. RET_SUBMIT responses are similarly sized. The TCP stack needs to handle these efficiently.

### Current Settings (verified correct)
| Setting | Value | Rationale |
|---------|-------|-----------|
| `TCP_SND_BUF` | 32768 | 2x max USB/IP frame, allows pipelining |
| `TCP_WND` | 32768 | Matching receive window |
| `TCP_MSS` | 1460 | Max for Ethernet (1500 - 40 headers) |
| `TCP_SND_QUEUELEN` | 64 | Enough segments for 2x 16KB frames |
| `TCP_RECVMBOX_SIZE` | 32 | Buffer incoming segments |
| `TCP_OVERSIZE_MSS` | yes | Allocate full MSS segments |
| `TCP_NODELAY` | per-socket | Disable Nagle for latency |
| `TCP_RTO_TIME` | 300ms | Fast retransmit on LAN |
| `IRAM_OPTIMIZATION` | yes | Hot path in fast memory |

### Additional Tuning (new)
| Setting | Value | Rationale |
|---------|-------|-----------|
| `LWIP_TCP_SACK_OUT` | yes | Selective ACK for faster recovery on segment loss |
| `LWIP_NETIF_TX_SINGLE_PBUF` | yes | Avoid pbuf chains for TX, reduces copy overhead |

These are already close to optimal for USB/IP over 100 Mbps Ethernet. The 32KB windows can sustain ~16KB USB/IP frames with one frame in-flight and one being assembled. TCP_NODELAY ensures small protocol headers (48-byte RET_SUBMIT with no data) are sent immediately.

---

## 12. Constraints

- All assets served from firmware flash (zero external dependencies)
- Works without internet
- No build step for frontend (vanilla HTML/CSS/JS)
- SVG topology rendered inline (no library)
- Canvas chart drawn with vanilla JS (no library)
- Must fit in existing app partition (78% free, ~2.4 MB available)
- WebSocket stats push at 500ms interval
- Max 2 concurrent WebSocket clients
- Responsive: mobile-usable down to 480px width
