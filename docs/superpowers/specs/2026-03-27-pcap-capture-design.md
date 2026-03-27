# PCAP Capture Feature Design Spec

**Date:** 2026-03-27
**Status:** Draft
**Component:** `capture`

## Overview

Add packet capture capability to the USB/IP server on ESP32-P4-Nano. All USB/IP protocol traffic (CMD_SUBMIT, RET_SUBMIT, CMD_UNLINK, RET_UNLINK including payloads) is recorded to a .pcap file on an SD card inserted into the board's SDIO 3.0 slot. The capture can be controlled and downloaded through the existing WebUI and REST API.

## Hardware

The ESP32-P4-Nano has a 4-wire SDIO 3.0 card slot:

| Signal | GPIO |
|--------|------|
| CLK    | 43   |
| CMD    | 44   |
| D0     | 39   |
| D1     | 40   |
| D2     | 41   |
| D3     | 42   |

- Bus width: 4-bit
- Clock: 40 MHz (high-speed mode)
- Internal pull-ups enabled on CMD and D0-D3

## Architecture

```
transfer_engine.c                   capture component
┌──────────────────┐          ┌──────────────────────────┐
│                  │          │  capture.h (public API)   │
│ handle_cmd_submit│──copy──▶ │                          │
│                  │  header  │  ┌──────────────────┐    │
│ send_completed   │──copy──▶ │  │  Ring Buffer     │    │
│    _reply        │  header  │  │  (PSRAM, 1 MB)   │    │
│                  │  +data   │  └────────┬─────────┘    │
│ handle_cmd_unlink│──copy──▶ │           │              │
│                  │          │  ┌────────▼─────────┐    │
│ (RET_UNLINK sent)│──copy──▶ │  │  Writer Task     │    │
│                  │          │  │  (Core 1, prio 2) │    │
└──────────────────┘          │  └────────┬─────────┘    │
                              │           │              │
                              │  ┌────────▼─────────┐    │
                              │  │  SD Card / FAT   │    │
                              │  │  SDMMC 4-bit     │    │
                              │  └──────────────────┘    │
                              │                          │
                              │  capture_api.c           │
                              │  (HTTP handlers)         │
                              └──────────────────────────┘
```

## Component Structure

```
components/capture/
├── CMakeLists.txt
├── include/
│   └── capture.h          # Public API
├── capture.c              # SD card init, PCAP file I/O, ring buffer, writer task
├── capture_hook.h         # Inline/macro capture hook for transfer_engine
└── capture_api.c          # HTTP endpoint handlers for WebUI
```

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "capture.c" "capture_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES driver sdmmc fatfs esp_timer vfs
)
```

The `transfer_engine` component will add `capture` to its `PRIV_REQUIRES`. The `webui` component will add `capture` to its `PRIV_REQUIRES` and call `capture_api_register(server)` during route setup.

## Public API (`capture.h`)

```c
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>
#include <stdbool.h>

/* Capture packet direction */
typedef enum {
    CAPTURE_DIR_CLIENT_TO_SERVER = 0,  /* CMD_SUBMIT, CMD_UNLINK */
    CAPTURE_DIR_SERVER_TO_CLIENT = 1,  /* RET_SUBMIT, RET_UNLINK */
} capture_direction_t;

/* Capture status (returned by capture_get_status) */
typedef struct {
    bool card_present;       /* SD card detected and mounted */
    bool capturing;          /* Currently recording */
    char filename[32];       /* Current/last capture filename */
    uint32_t file_size;      /* Bytes written to .pcap file */
    uint32_t packet_count;   /* Number of PCAP records written */
    uint32_t dropped_count;  /* Packets dropped (ring buffer full) */
    uint32_t ring_buf_used;  /* Bytes currently in ring buffer */
    uint32_t ring_buf_size;  /* Total ring buffer capacity */
} capture_status_t;

/* Initialize the capture subsystem (SD card, mount FAT) */
esp_err_t capture_init(void);

/* Start capturing to a new timestamped .pcap file */
esp_err_t capture_start(void);

/* Stop capturing and flush remaining data */
esp_err_t capture_stop(void);

/* Get current status */
esp_err_t capture_get_status(capture_status_t *out);

/* Submit a packet to the capture ring buffer (called from transfer_engine).
 * This must be non-blocking and safe to call from the transfer engine context.
 * If the ring buffer is full, the packet is silently dropped. */
void capture_submit_packet(capture_direction_t dir,
                           const void *hdr, uint32_t hdr_len,
                           const void *payload, uint32_t payload_len);

/* Delete the current/last capture file */
esp_err_t capture_delete_file(void);

/* Get the full filesystem path of the current capture file (for HTTP serving) */
const char *capture_get_filepath(void);

/* Register HTTP API endpoints on the given server */
void capture_api_register(httpd_handle_t server);
```

## PCAP File Format

### Global Header (24 bytes)

Written once at the start of each capture file:

| Field              | Value              | Description                    |
|--------------------|--------------------|--------------------------------|
| magic_number       | `0xa1b2c3d4`       | Standard PCAP, microsecond ts  |
| version_major      | `2`                |                                |
| version_minor      | `4`                |                                |
| thiszone           | `0`                | GMT                            |
| sigfigs            | `0`                |                                |
| snaplen            | `65535`            | Max capture length             |
| network            | `220` (LINKTYPE_USB_LINUX_MMAPPED) | Linux usbmon format, native Wireshark support |

**Rationale for LINKTYPE_USB_LINUX_MMAPPED (220):** Wireshark has native dissection for this format. Each packet uses a 64-byte pseudo-header (`struct mon_bin_hdr`) that maps directly from USB/IP fields:

| Offset | Size | Field | USB/IP mapping |
|--------|------|-------|----------------|
| 0 | 8 | id | seqnum (used as URB ID) |
| 8 | 1 | type | 'S' for CMD_SUBMIT, 'C' for RET_SUBMIT |
| 9 | 1 | transfer_type | 0=ISO,1=INT,2=CTRL,3=BULK (from endpoint/setup) |
| 10 | 1 | endpoint | ep number with direction bit 7 |
| 11 | 1 | device | dev_addr |
| 12 | 2 | bus_id | 1 |
| 14 | 1 | setup_flag | 0 if control with setup, else 0xFF |
| 15 | 1 | data_flag | 0 if data present, else 1 |
| 16 | 8 | ts_sec + ts_usec | esp_timer microseconds split |
| 24 | 4 | status | 0 for submit, Linux errno for complete |
| 28 | 4 | urb_len | transfer_buffer_length |
| 32 | 4 | data_len | actual captured data length |
| 36 | 8 | setup[8] | setup packet (control only) |
| 44 | 4 | interval | interval field |
| 48 | 4 | start_frame | start_frame field |
| 52 | 4 | xfer_flags | transfer_flags |
| 56 | 4 | ndesc | number_of_packets (ISO) |
| 60 | 4 | padding | 0 |

This gives full Wireshark USB analysis: device/endpoint filtering, transfer type coloring, setup packet decoding, payload hex dump, request/response pairing via URB ID.

### Packet Record

Each captured packet uses the standard 16-byte PCAP record header followed by a 64-byte Linux USB pseudo-header and then payload data:

```
[16 bytes: pcap_rec_hdr] [64 bytes: mon_bin_hdr] [N bytes: USB data payload]
```

The `mon_bin_hdr` is populated by mapping USB/IP protocol fields as described in the link type table above. This allows Wireshark to fully decode the USB traffic natively.

### Snap Length

To keep file sizes manageable, the capture will truncate packet payloads at a configurable snap length (default: 256 bytes of payload beyond the 48-byte USB/IP header). The full original length is recorded in `orig_len` so analysis tools know data was truncated. The 48-byte USB/IP header is always captured in full.

Config constant: `CONFIG_CAPTURE_SNAPLEN` (Kconfig, default 256).

## Ring Buffer Design

### Layout

A single contiguous buffer allocated in PSRAM. Default size: 1 MB (`CONFIG_CAPTURE_RING_BUF_SIZE`).

```c
typedef struct {
    uint8_t *buf;           /* PSRAM allocation */
    uint32_t size;          /* Total buffer size */
    volatile uint32_t head; /* Write position (transfer_engine writes) */
    volatile uint32_t tail; /* Read position (writer task reads) */
    portMUX_TYPE lock;      /* Spinlock for head/tail updates */
} capture_ring_t;
```

Each entry in the ring buffer is framed as:

```
[4 bytes: entry_len] [16 bytes: pcap_record_hdr] [64 bytes: mon_bin_hdr] [N bytes: payload]
```

The `entry_len` field allows the writer task to read complete entries without parsing PCAP headers.

### Producer (transfer_engine context)

`capture_submit_packet()` is called from the transfer engine's main loop (single-threaded per connection). It:

1. Computes total entry size: `4 + 16 + 4 + hdr_len + min(payload_len, snaplen)`
2. Enters spinlock
3. Checks available space: `(size - ((head - tail) % size))`
4. If insufficient space: increments `dropped_count`, exits spinlock, returns
5. Copies the entry (entry_len, pcap header, direction word, USB/IP header, truncated payload) into the ring buffer using memcpy with wrap-around handling
6. Advances `head`
7. Exits spinlock

The spinlock hold time is bounded by the memcpy of at most ~320 bytes (4 + 16 + 4 + 48 + 256), which at ESP32-P4's 360 MHz takes under 1 microsecond from PSRAM.

### Consumer (writer task)

A dedicated FreeRTOS task (`capture_writer_task`) pinned to Core 1 at priority 2 (just above idle):

1. Loop: check if `head != tail`
2. If data available: read `entry_len`, then read the full entry
3. Write to the open PCAP file via `fwrite()`
4. Advance `tail` (under spinlock)
5. If no data available: `vTaskDelay(pdMS_TO_TICKS(10))` to yield

The writer task opens the file at capture start and closes it at capture stop. It uses a local 4 KB stack buffer to batch small writes before issuing `fwrite()`, reducing SD card write overhead.

### Overflow Policy

When the ring buffer is full, new packets are silently dropped. The transfer engine is never blocked. The `dropped_count` is visible in the status API and WebUI so the user knows data was lost.

## SD Card Management

### Initialization (`capture_init`)

1. Configure SDMMC host:
   ```c
   sdmmc_host_t host = SDMMC_HOST_DEFAULT();
   host.max_freq_khz = 40000;  // 40 MHz

   sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
   slot.width = 4;
   slot.clk = GPIO_NUM_43;
   slot.cmd = GPIO_NUM_44;
   slot.d0  = GPIO_NUM_39;
   slot.d1  = GPIO_NUM_40;
   slot.d2  = GPIO_NUM_41;
   slot.d3  = GPIO_NUM_42;
   slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
   ```
2. Mount FAT filesystem at `/sdcard` using `esp_vfs_fat_sdmmc_mount()`
3. If no card: set `card_present = false`, return `ESP_OK` (non-fatal)
4. If mount succeeds: set `card_present = true`

### File Naming

Capture files are named: `/sdcard/cap_NNNN.pcap` where `NNNN` is a zero-padded monotonic counter (0000-9999). On `capture_start`, the component scans `/sdcard/cap_*.pcap` to find the next available number, or wraps to 0000 if all are used.

Since the ESP32-P4 has no battery-backed RTC, true timestamps are not available for filenames. The monotonic counter is more reliable. Timestamps within the PCAP file use `esp_timer_get_time()` (microseconds since boot), converted to seconds + microseconds.

### Card Hot-Removal

If `fwrite()` returns an error during capture, the writer task:

1. Sets `card_present = false`
2. Stops the capture (sets `capturing = false`)
3. Logs an event via `event_log_add()`
4. Unmounts the FAT filesystem

Re-insertion is not auto-detected (no card-detect GPIO is specified). The user must call `capture_init()` again (exposed as a "Reinitialize SD" button in the WebUI, or automatic on next `capture_start` attempt).

## Transfer Engine Hooks

### Where to Hook

Four capture points in `transfer_engine.c`:

1. **CMD_SUBMIT received** (line ~648 area, after `usbip_pack_header(&hdr, false)`):
   Capture the unpacked header. For OUT transfers, also capture the payload data after it has been received from the socket (line ~450 area, after `usbip_net_recv` of OUT data).

2. **RET_SUBMIT sent** (in `send_completed_reply`, before the `usbip_net_send` calls):
   Capture the reply header and, for IN transfers, the payload data.

3. **CMD_UNLINK received** (line ~502 area, after header unpack):
   Capture the unpacked header (no payload).

4. **RET_UNLINK sent** (line ~537 area, before `usbip_net_send`):
   Capture the reply header (no payload).

### Hook Implementation

Each hook is a single function call. The hooks capture the **host-byte-order** header (post-unpack for incoming, pre-pack for outgoing) so the PCAP file contains consistently-formatted data that is easier to analyze. The direction flag distinguishes client-to-server from server-to-client.

Example hook placement in `handle_cmd_submit`:

```c
/* After usbip_pack_header(&hdr, false) and receiving OUT data */
capture_submit_packet(CAPTURE_DIR_CLIENT_TO_SERVER,
                      &hdr, sizeof(hdr),
                      (direction == USBIP_DIR_OUT && buflen > 0) ? dest : NULL,
                      (direction == USBIP_DIR_OUT && buflen > 0) ? buflen : 0);
```

Example hook in `send_completed_reply`:

```c
/* Before packing the reply header for network send */
capture_submit_packet(CAPTURE_DIR_SERVER_TO_CLIENT,
                      &reply, sizeof(reply),
                      (slot->direction == USBIP_DIR_IN && actual_length > 0) ? src : NULL,
                      (slot->direction == USBIP_DIR_IN && actual_length > 0) ? actual_length : 0);
```

### Conditional Compilation

The capture hooks are guarded by `#if CONFIG_CAPTURE_ENABLED` (Kconfig boolean, default `y`). When disabled, the hooks compile to nothing and the capture component is not linked.

## HTTP API Endpoints

All endpoints require authentication (if enabled) via the existing `webui_check_auth` mechanism.

### GET /api/capture/status

Returns JSON:

```json
{
  "card_present": true,
  "capturing": false,
  "filename": "cap_0003.pcap",
  "file_size": 1048576,
  "packet_count": 4231,
  "dropped_count": 0,
  "ring_buf_used": 0,
  "ring_buf_size": 1048576
}
```

### POST /api/capture/start

Starts a new capture. Returns `{"ok": true, "filename": "cap_0004.pcap"}` or error.

If already capturing, returns 400 error. If no card present, attempts re-initialization first.

### POST /api/capture/stop

Stops capture, flushes ring buffer to disk. Returns `{"ok": true}`.

### GET /api/capture/download

Streams the .pcap file as `application/octet-stream` with `Content-Disposition: attachment; filename="cap_0003.pcap"`. Uses chunked transfer encoding, reading 4 KB at a time from the file.

If capture is active, the file is flushed before serving (writer task is signaled to flush, then the file is served). This means the download is a snapshot -- packets arriving during download are not included.

### POST /api/capture/delete

Deletes the current capture file. Returns 400 if capture is active (must stop first).

## WebUI Integration

### Dashboard Addition

A new "Packet Capture" panel is added to the Dashboard section (`index.html`), placed after the "Recent Events" panel:

```html
<div class="panel">
    <div class="panel-title">Packet Capture</div>
    <div class="capture-status">
        <div class="info-row">
            <span>SD Card</span>
            <span id="cap-card" class="status-badge">--</span>
        </div>
        <div class="info-row">
            <span>Status</span>
            <span id="cap-status" class="status-badge">--</span>
        </div>
        <div class="info-row">
            <span>File</span>
            <span id="cap-filename">--</span>
        </div>
        <div class="info-row">
            <span>Size</span>
            <span id="cap-size">--</span>
        </div>
        <div class="info-row">
            <span>Packets</span>
            <span id="cap-packets">--</span>
        </div>
        <div class="info-row">
            <span>Dropped</span>
            <span id="cap-dropped">--</span>
        </div>
    </div>
    <div class="btn-row">
        <button class="btn" id="cap-toggle" onclick="toggleCapture()">Start Capture</button>
        <button class="btn" id="cap-download" onclick="downloadCapture()" disabled>Download</button>
        <button class="btn btn-danger" id="cap-delete" onclick="deleteCapture()" disabled>Delete</button>
    </div>
</div>
```

### JavaScript Logic (`app.js` additions)

- Poll `GET /api/capture/status` every 2 seconds (same interval as the existing WebSocket stats) and update the UI elements.
- `toggleCapture()`: POST to `/api/capture/start` or `/api/capture/stop` depending on current state.
- `downloadCapture()`: navigate to `/api/capture/download` (browser handles the download).
- `deleteCapture()`: POST to `/api/capture/delete` with confirmation dialog.
- Disable Start when no card present. Disable Download/Delete when no file exists.
- Show dropped count in warning color (orange) if > 0.

### WebSocket Integration (Optional Enhancement)

The capture status could be included in the existing WebSocket broadcast payload (`ws_handler.c`) to avoid separate polling. This is a minor optimization and can be deferred to implementation.

## Kconfig Options

Added under a `menu "Capture"` in the component's Kconfig:

| Option                        | Type   | Default    | Description                          |
|-------------------------------|--------|------------|--------------------------------------|
| `CONFIG_CAPTURE_ENABLED`      | bool   | `y`        | Enable packet capture support        |
| `CONFIG_CAPTURE_RING_BUF_SIZE`| int    | `1048576`  | Ring buffer size in bytes (PSRAM)    |
| `CONFIG_CAPTURE_SNAPLEN`      | int    | `256`      | Max payload bytes per packet record  |
| `CONFIG_CAPTURE_WRITER_STACK` | int    | `4096`     | Writer task stack size               |

## Resource Estimates

### Flash

| Item                    | Size     |
|-------------------------|----------|
| capture.c code          | ~4 KB    |
| capture_api.c code      | ~3 KB    |
| HTML additions          | ~1 KB    |
| JS additions            | ~2 KB    |
| **Total flash**         | **~10 KB** |

### RAM

| Item                        | Location  | Size     |
|-----------------------------|-----------|----------|
| Ring buffer                 | PSRAM     | 1 MB (configurable) |
| Writer task stack           | Internal  | 4 KB     |
| SD card DMA buffers         | Internal  | ~8 KB (SDMMC driver) |
| FAT filesystem workbuf      | Internal  | ~4 KB    |
| Static variables            | Internal  | ~256 B   |
| **Total internal SRAM**     |           | **~16 KB** |
| **Total PSRAM**             |           | **1 MB** |

The ESP32-P4 has 32 MB PSRAM (at 200 MHz hex-SPI per sdkconfig), so 1 MB is roughly 3% of PSRAM. The 16 KB internal SRAM overhead is modest given the `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=256000` setting.

### SD Card Write Bandwidth

At 40 MHz 4-bit SDIO, theoretical throughput is ~20 MB/s. Practical FAT write throughput with a decent SD card is 5-15 MB/s. USB/IP traffic on a Full-Speed device peaks at ~1.2 MB/s (12 Mbps); on a High-Speed device it peaks at ~50 MB/s (480 Mbps), though the Ethernet link is the bottleneck at ~95 Mbps (~11 MB/s). With snap length truncation at 256 bytes, the actual capture data rate will be much lower than raw throughput since each packet record is at most ~324 bytes regardless of transfer size.

**Worst case estimate:** 10,000 transfers/second x 324 bytes = ~3.2 MB/s capture rate. This is well within SD card write capability. The 1 MB ring buffer provides ~300 ms of buffering at this rate, which is sufficient to absorb SD card write latency spikes (FAT cluster allocation, wear leveling).

### CPU Overhead

The capture hooks add one `capture_submit_packet()` call per USB/IP command/response. The function body is a spinlock-protected memcpy of ~320 bytes maximum. At 360 MHz, this takes under 2 microseconds per call. The writer task runs on Core 1 at low priority, consuming CPU only when flushing data. Impact on USB/IP transfer latency is negligible.

## Data Flow Walkthrough

### Capture of a CMD_SUBMIT + RET_SUBMIT (IN transfer)

1. Client sends CMD_SUBMIT header (48 bytes) over TCP
2. `transfer_engine_run` receives and unpacks the header
3. `handle_cmd_submit` is called
4. **Hook 1:** `capture_submit_packet(CLIENT_TO_SERVER, &hdr, 48, NULL, 0)` -- no payload for IN
5. Transfer is submitted to USB host, completes
6. `send_completed_reply` builds the RET_SUBMIT header and reads IN data from the USB transfer buffer
7. **Hook 2:** `capture_submit_packet(SERVER_TO_CLIENT, &reply, 48, src, actual_length)` -- includes IN payload (truncated to snaplen)
8. Reply is packed to network byte order and sent over TCP

### Capture of a CMD_SUBMIT + RET_SUBMIT (OUT transfer)

1. Client sends CMD_SUBMIT header (48 bytes) + OUT payload over TCP
2. `handle_cmd_submit` receives header, unpacks it, then receives OUT data
3. **Hook 1:** `capture_submit_packet(CLIENT_TO_SERVER, &hdr, 48, dest, buflen)` -- includes OUT payload (truncated to snaplen)
4. Transfer completes
5. `send_completed_reply` builds the RET_SUBMIT header (no IN data for OUT transfers)
6. **Hook 2:** `capture_submit_packet(SERVER_TO_CLIENT, &reply, 48, NULL, 0)`
7. Reply sent

## File List Summary

New files:

| File | Purpose |
|------|---------|
| `components/capture/CMakeLists.txt` | Component build definition |
| `components/capture/Kconfig` | Configurable options |
| `components/capture/include/capture.h` | Public API |
| `components/capture/capture.c` | Core: SD init, ring buffer, writer task, PCAP format |
| `components/capture/capture_api.c` | HTTP endpoint handlers |

Modified files:

| File | Change |
|------|--------|
| `components/transfer_engine/CMakeLists.txt` | Add `capture` to PRIV_REQUIRES |
| `components/transfer_engine/transfer_engine.c` | Add 4 capture hook calls |
| `components/webui/CMakeLists.txt` | Add `capture` to PRIV_REQUIRES |
| `components/webui/webui_api.c` | Call `capture_api_register(server)` |
| `components/webui/frontend/index.html` | Add Packet Capture panel to Dashboard |
| `components/webui/frontend/app.js` | Add capture status polling and control functions |
| `main/main.c` (or equivalent) | Call `capture_init()` during startup |

## Open Questions

1. **Card-detect GPIO:** The requirements specify pin assignments for the SDIO data bus but not a card-detect (CD) pin. If a CD pin is available, hot-insertion could be supported with a GPIO interrupt. Without it, card presence is only checked at init/start time.

2. **Wireshark dissector:** Resolved -- using LINKTYPE_USB_LINUX_MMAPPED (220) which Wireshark decodes natively as USB traffic. No custom dissector needed.

3. **Multi-device capture:** The current design captures all USB/IP traffic on the single server socket. If multiple devices are exported simultaneously (multiple `transfer_engine_run` instances), each instance's hooks independently submit packets to the same ring buffer. The `devid` field in the USB/IP header distinguishes devices within the capture file. No changes needed for this to work.

4. **Capture file rotation:** The current design creates a single file per capture session. Automatic rotation (e.g., when file exceeds N MB) could be added later by having the writer task close the current file and open a new one when a size threshold is reached.
