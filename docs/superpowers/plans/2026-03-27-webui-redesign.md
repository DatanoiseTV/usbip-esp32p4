# WebUI Redesign + Protocol Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the basic dark dashboard with a professional UniFi-style management interface featuring live USB topology, detailed device info, WebUI auth, and drop v1.0.0 protocol support.

**Architecture:** Single-page app with sidebar navigation (Dashboard/Devices/Topology/Settings). Backend extended with REST API endpoints, auth middleware, device descriptor storage. Frontend is vanilla HTML/CSS/JS with no external dependencies.

**Tech Stack:** ESP-IDF v5.5, esp_http_server, WebSocket, mbedTLS SHA-256, vanilla JS, inline SVG, Canvas API.

**Working directory:** `/Users/dev/dev/usbip-esp32p4`
**Build:** `source /Users/dev/.espressif/v5.5.3/esp-idf/export.sh && cd /Users/dev/dev/usbip-esp32p4 && idf.py build`

---

## File Structure

### Files to modify
```
components/usbip_proto/include/usbip_proto.h    # Remove V100 constant
components/usbip_proto/usbip_proto.c             # V111 only in version check
components/device_manager/include/device_manager.h # Add descriptor fields
components/device_manager/device_manager.c        # Store descriptors
components/usb_host_mgr/usb_host_mgr.c           # Read string descriptors, config desc
components/usb_host_mgr/include/usb_host_mgr.h   # Add reset_device API
components/webui/CMakeLists.txt                   # Update EMBED_FILES
components/webui/webui.c                          # Auth middleware, new routes
components/webui/ws_handler.c                     # Extended stats payload
components/webui/include/webui.h                  # Update declarations
sdkconfig.defaults                                # lwIP SACK + single pbuf
```

### Files to create
```
components/webui/webui_api.c                      # REST API handlers
components/webui/frontend/index.html              # Full SPA (replaces old)
components/webui/frontend/style.css               # Light theme (replaces old)
components/webui/frontend/app.js                  # All JS logic (replaces old htmx.min.js)
```

### Files to delete
```
components/webui/frontend/htmx.min.js             # Replaced by app.js
components/webui/frontend/settings.html           # Merged into index.html SPA
```

---

## Task 1: Drop v1.0.0 Protocol + lwIP Tuning

**Files:**
- Modify: `components/usbip_proto/include/usbip_proto.h`
- Modify: `components/usbip_proto/usbip_proto.c`
- Modify: `sdkconfig.defaults`

- [ ] **Step 1: Remove v1.0.0 from protocol header**

In `usbip_proto.h`, remove the `USBIP_VERSION_V100` define and update `usbip_version_supported` doc comment to say v1.1.1 only.

- [ ] **Step 2: Update version check to v1.1.1 only**

In `usbip_proto.c`, change `usbip_version_supported()`:
```c
bool usbip_version_supported(uint16_t version)
{
    return (version == USBIP_VERSION_V111);
}
```

- [ ] **Step 3: Add lwIP SACK and single-pbuf TX to sdkconfig.defaults**

Append to the lwIP section:
```ini
CONFIG_LWIP_TCP_SACK_OUT=y
CONFIG_LWIP_NETIF_TX_SINGLE_PBUF=y
```

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 2: Extend Device Manager with Descriptors

**Files:**
- Modify: `components/device_manager/include/device_manager.h`
- Modify: `components/device_manager/device_manager.c`

- [ ] **Step 1: Add descriptor fields to dm_device_info_t**

Add to the struct in `device_manager.h`:
```c
    /* String descriptors (UTF-8, null-terminated) */
    char     manufacturer[64];
    char     product[64];
    char     serial[64];

    /* Raw config descriptor (for endpoint/interface detail parsing) */
    uint8_t *config_desc_raw;        /**< Pointer to raw config descriptor (PSRAM) */
    uint16_t config_desc_len;        /**< Length of raw config descriptor */

    /* Transfer statistics */
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint32_t urbs_completed;
    uint32_t urbs_failed;
    int64_t  last_activity_us;
```

- [ ] **Step 2: Update device_manager_remove to free config descriptor**

In `device_manager_remove()`, before zeroing the slot, free the config descriptor:
```c
if (dev->config_desc_raw) {
    free(dev->config_desc_raw);
    dev->config_desc_raw = NULL;
}
```

- [ ] **Step 3: Add stats update function**

Add to header:
```c
void device_manager_update_stats(int index, uint32_t bytes_in_delta,
                                  uint32_t bytes_out_delta, bool success);
```

Implement: takes lock, adds deltas to bytes_in/bytes_out, increments urbs_completed or urbs_failed, updates last_activity_us.

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 3: USB Host Manager - Read Descriptors + Reset

**Files:**
- Modify: `components/usb_host_mgr/usb_host_mgr.c`
- Modify: `components/usb_host_mgr/include/usb_host_mgr.h`

- [ ] **Step 1: Read string descriptors during enumeration**

In `handle_new_device()`, after reading the device descriptor and before calling `device_manager_add()`, read the string descriptors using control transfers:

For each string index (iManufacturer, iProduct, iSerialNumber from the device descriptor):
1. If index > 0, do a control transfer to read the string descriptor (GET_DESCRIPTOR, type=STRING, language=0x0409 English)
2. Convert UTF-16LE to UTF-8 (simple: take every other byte for ASCII range)
3. Store in the `dm_device_info_t` manufacturer/product/serial fields

Use `usb_host_transfer_alloc()` + `usb_host_transfer_submit_control()` with a semaphore for synchronous operation (same pattern as enumeration control transfers).

- [ ] **Step 2: Store raw config descriptor**

After `usb_host_get_active_config_descriptor()`, allocate PSRAM buffer and copy the raw config descriptor:
```c
info.config_desc_raw = heap_caps_malloc(cfg_desc->wTotalLength, MALLOC_CAP_SPIRAM);
if (info.config_desc_raw) {
    memcpy(info.config_desc_raw, cfg_desc, cfg_desc->wTotalLength);
    info.config_desc_len = cfg_desc->wTotalLength;
}
```

- [ ] **Step 3: Add device reset function**

Add to header:
```c
esp_err_t usb_host_mgr_reset_device(uint8_t dev_addr);
```

Implement using `usb_host_device_reset()` if available in ESP-IDF v5.5, or by closing and re-opening the device. If the API doesn't exist, log a warning and return `ESP_ERR_NOT_SUPPORTED`.

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 4: Transfer Engine - Update Device Stats

**Files:**
- Modify: `components/transfer_engine/transfer_engine.c`

- [ ] **Step 1: Call device_manager_update_stats on transfer completion**

In `send_completed_reply()`, after computing actual_length and reply_status, call:
```c
device_manager_update_stats(dev_index,
    (slot->direction == USBIP_DIR_IN) ? actual_length : 0,
    (slot->direction == USBIP_DIR_OUT) ? slot->buflen : 0,
    reply_status == 0);
```

This requires passing `dev_index` into the function or storing it in the pending_urb_t. Add `int dev_index` to the `pending_urb_t` struct and set it when the transfer engine starts.

- [ ] **Step 2: Build and verify**

- [ ] **Step 3: Commit**

---

## Task 5: WebUI Authentication

**Files:**
- Modify: `components/webui/webui.c`
- Modify: `components/webui/include/webui.h`
- Modify: `components/webui/CMakeLists.txt`

- [ ] **Step 1: Add auth state and NVS load/save**

Add to webui.c:
```c
#include "mbedtls/sha256.h"
#include "nvs_flash.h"
#include "nvs.h"

#define AUTH_NVS_NAMESPACE "auth"
#define AUTH_MAX_USERNAME 32
#define AUTH_MAX_PASSWORD 64

static bool s_auth_enabled = false;
static char s_auth_username[AUTH_MAX_USERNAME] = "";
static uint8_t s_auth_password_hash[32] = {0};

static void auth_load_from_nvs(void) { /* read enabled, username, password_hash from NVS */ }
static void auth_save_to_nvs(void) { /* write enabled, username, password_hash to NVS */ }
static void auth_hash_password(const char *password, uint8_t out_hash[32]) {
    mbedtls_sha256((const uint8_t *)password, strlen(password), out_hash, 0);
}
```

- [ ] **Step 2: Add Basic Auth check function**

```c
static bool auth_check_request(httpd_req_t *req)
{
    if (!s_auth_enabled) return true;

    char auth_header[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    /* Parse "Basic base64(user:pass)", decode, split on ':', hash password, compare */
    /* Use mbedtls_base64_decode for base64 */
    /* Compare username and SHA-256 hash */
}

static esp_err_t auth_reject(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"USB/IP Server\"");
    httpd_resp_send(req, "Unauthorized", 12);
    return ESP_OK;
}
```

- [ ] **Step 3: Add auth middleware to all HTTP handlers**

At the top of every handler, add:
```c
if (!auth_check_request(req)) return auth_reject(req);
```

- [ ] **Step 4: Add mbedtls to CMakeLists.txt PRIV_REQUIRES**

- [ ] **Step 5: Build and verify**

- [ ] **Step 6: Commit**

---

## Task 6: REST API Endpoints

**Files:**
- Create: `components/webui/webui_api.c`
- Modify: `components/webui/CMakeLists.txt` (add to SRCS)
- Modify: `components/webui/webui.c` (register routes)

- [ ] **Step 1: Create webui_api.c with device detail endpoint**

`GET /api/devices/{index}` - returns JSON with full device info including:
- Basic info (vid, pid, speed, state, busid, manufacturer, product, serial)
- Parsed interfaces from raw config descriptor (interface class/subclass/protocol)
- Parsed endpoints from raw config descriptor (address, direction, type, maxpacketsize, interval)
- Transfer stats (bytes_in, bytes_out, urbs_ok, urbs_fail, last_activity)

Parse the raw config descriptor in C: walk the descriptor buffer looking for INTERFACE and ENDPOINT descriptor types, extract fields, build JSON.

- [ ] **Step 2: Add device action endpoints**

`POST /api/devices/{index}/reset` - calls `usb_host_mgr_reset_device()`
`POST /api/devices/{index}/disconnect` - calls `device_manager_release()`
`POST /api/devices/{index}/export` - toggles export state

Each returns `{"ok":true}` or `{"ok":false,"error":"message"}`.

- [ ] **Step 3: Add settings endpoints**

`GET /api/settings/network` - returns current network config from NVS
`POST /api/settings/network` - saves hostname, dhcp, static ip etc. to NVS
`GET /api/settings/auth` - returns `{"enabled":bool,"username":"..."}`
`POST /api/settings/auth` - saves auth settings, hashes password
`GET /api/settings/acl` - returns access control state
`POST /api/settings/acl` - saves access control settings

- [ ] **Step 4: Add system endpoints**

`POST /api/system/restart` - calls `esp_restart()` after 500ms delay
`POST /api/system/factory-reset` - erases NVS then restarts
`GET /api/system/info` - returns firmware version, chip info, heap stats

- [ ] **Step 5: Register all routes in webui.c**

- [ ] **Step 6: Build and verify**

- [ ] **Step 7: Commit**

---

## Task 7: WebSocket Stats Extension

**Files:**
- Modify: `components/webui/ws_handler.c`

- [ ] **Step 1: Extend stats JSON with device details**

Update `ws_broadcast_stats()` to include per-device:
- manufacturer, product strings
- bytes_in, bytes_out (for throughput calculation)
- urbs_completed, urbs_failed
- config_desc_len (so frontend knows detail is available)
- All interface class codes (for quick topology display)

The JSON structure:
```json
{
  "type": "stats",
  "free_heap": 481000,
  "free_internal": 240000,
  "uptime_sec": 3600,
  "devices": [
    {
      "idx": 0,
      "path": "1-1",
      "vid": 42086,
      "pid": 2643,
      "speed": 1,
      "state": 1,
      "client_ip": 3232235880,
      "manufacturer": "ACME Corp",
      "product": "IC Programmer",
      "bytes_in": 1048576,
      "bytes_out": 524288,
      "urbs_ok": 15000,
      "urbs_fail": 2,
      "has_detail": true,
      "iface_classes": [255]
    }
  ],
  "logs": [...]
}
```

- [ ] **Step 2: Build and verify**

- [ ] **Step 3: Commit**

---

## Task 8: Frontend - HTML Structure + CSS

**Files:**
- Create: `components/webui/frontend/index.html` (replace old)
- Create: `components/webui/frontend/style.css` (replace old)
- Delete: `components/webui/frontend/settings.html`
- Delete: `components/webui/frontend/htmx.min.js`
- Modify: `components/webui/CMakeLists.txt` (update EMBED_FILES)

- [ ] **Step 1: Write index.html with full SPA structure**

Single HTML file containing:
- Fixed sidebar with nav links (Dashboard, Devices, Topology, Settings)
- Four main content sections (shown/hidden via JS)
- Dashboard: stat cards, bandwidth chart canvas, event log
- Devices: device list container, empty state
- Topology: SVG container
- Settings: network form, auth form, ACL form, system info
- Script tag linking to app.js

All sections present in HTML, only active section visible (display:block, others display:none).

- [ ] **Step 2: Write style.css with UniFi light theme**

Full stylesheet implementing the design spec:
- CSS variables (light theme colors)
- Sidebar (dark, fixed, 220px)
- Cards (white, shadow, rounded)
- Stats row (flexbox, 5 cards)
- Device list (cards with expand/collapse)
- Topology SVG styling
- Form styling (inputs, buttons, toggles)
- Responsive breakpoints (768px, 480px)
- Speed badges, status dots, action buttons

- [ ] **Step 3: Update CMakeLists.txt EMBED_FILES**

Remove `settings.html` and `htmx.min.js`, add `app.js`:
```cmake
EMBED_FILES
    "frontend/index.html"
    "frontend/style.css"
    "frontend/app.js"
```

Update webui.c embedded file symbols to match (remove settings_html and htmx references, add app_js).

- [ ] **Step 4: Build and verify**

- [ ] **Step 5: Commit**

---

## Task 9: Frontend - Dashboard + Sidebar JS

**Files:**
- Create: `components/webui/frontend/app.js`

- [ ] **Step 1: Write core app.js with WebSocket, navigation, and dashboard**

The JS file handles:
- **Sidebar navigation**: click handlers that show/hide sections
- **WebSocket connection**: connect, reconnect on close, parse JSON messages
- **Dashboard stats**: update stat card values from WS data
- **Bandwidth chart**: canvas drawing with 120-point rolling window
- **Event log**: render last 15 entries
- **Utility functions**: formatBytes, formatUptime, hex4, esc, ip4str, speedStr, stateStr

USB class code lookup table:
```js
var USB_CLASS = {
    0x00:'Composite',0x01:'Audio',0x02:'CDC',0x03:'HID',0x05:'Physical',
    0x06:'Image',0x07:'Printer',0x08:'Mass Storage',0x09:'Hub',
    0x0A:'CDC Data',0x0B:'Smart Card',0x0D:'Content Security',
    0x0E:'Video',0x0F:'Healthcare',0x10:'Audio/Video',0x11:'Billboard',
    0x12:'USB-C Bridge',0xDC:'Diagnostic',0xE0:'Wireless',
    0xEF:'Miscellaneous',0xFE:'Application Specific',0xFF:'Vendor Specific'
};
```

- [ ] **Step 2: Build and verify (dashboard should render)**

- [ ] **Step 3: Commit**

---

## Task 10: Frontend - Devices Section JS

**Files:**
- Modify: `components/webui/frontend/app.js`

- [ ] **Step 1: Add device list rendering with collapsed/expanded cards**

When WS stats arrive, render device cards. Each card shows:
- Collapsed: status dot, busid, VID:PID, speed badge, status, throughput, Details button
- Click Details: fetch `/api/devices/{idx}` via XMLHttpRequest
- Expanded: device info, interfaces table, endpoints table, stats, action buttons

Action button handlers:
- Export/Block: POST to `/api/devices/{idx}/export`
- Disconnect: POST to `/api/devices/{idx}/disconnect` with confirm()
- Reset: POST to `/api/devices/{idx}/reset` with confirm()

Interface/endpoint tables built from the `/api/devices/{idx}` JSON response which contains parsed config descriptor data.

- [ ] **Step 2: Build and verify**

- [ ] **Step 3: Commit**

---

## Task 11: Frontend - Topology Section JS

**Files:**
- Modify: `components/webui/frontend/app.js`

- [ ] **Step 1: Add SVG topology tree builder**

Build a tree data structure from the flat device list:
1. Parse busid strings: "1-1" = bus 1, port 1 (direct). "1-1.3" = bus 1, port 1 (hub), downstream port 3.
2. Group devices by parent (hub or root).
3. Render SVG:
   - Root node: "ESP32-P4 Host Controller" rectangle at top center
   - For each root-port device:
     - If hub (class 0x09): draw hub rectangle with port circles along bottom
     - If device: draw device rectangle with VID:PID, name, speed badge
   - Connecting lines: vertical from parent, right-angle to children
   - Color-coded borders: green=imported, blue=available, gray=blocked
   - Click handler: navigate to Devices section and expand that device

Layout: simple top-down. Host controller centered at top. Direct devices below. Hub ports spread horizontally with devices hanging below their ports.

SVG is inline in the topology section div, rebuilt on every device change.

- [ ] **Step 2: Build and verify**

- [ ] **Step 3: Commit**

---

## Task 12: Frontend - Settings Section JS

**Files:**
- Modify: `components/webui/frontend/app.js`

- [ ] **Step 1: Add settings form handlers**

**Network section:**
- On page load: GET `/api/settings/network`, populate form
- DHCP checkbox: toggles visibility of static IP fields
- Save button: POST `/api/settings/network` with form data, show "Saved. Reboot required." message

**Auth section:**
- On page load: GET `/api/settings/auth`, populate form
- Enable checkbox: toggles username/password fields
- Save button: POST `/api/settings/auth` with credentials

**ACL section:**
- On page load: GET `/api/settings/acl`, populate mode + IP list
- Mode radio: toggles allowlist visibility
- Add IP: POST to add, refresh list
- Remove IP: POST to remove, refresh list

**System section:**
- On page load: GET `/api/system/info`, display
- Free heap updated from WS stats
- Restart button: confirm() then POST `/api/system/restart`
- Factory Reset button: confirm() then POST `/api/system/factory-reset`

- [ ] **Step 2: Build and verify**

- [ ] **Step 3: Commit**

---

## Task 13: Integration + Final Build

**Files:**
- All modified files

- [ ] **Step 1: Full clean build**

```bash
idf.py fullclean && idf.py set-target esp32p4 && idf.py build
```

Fix any errors.

- [ ] **Step 2: Verify flash size**

Check that the binary fits in the app partition (should be well under 3MB).

- [ ] **Step 3: Flash and test**

```bash
idf.py -p /dev/cu.usbmodem5ABA0770311 flash monitor
```

Verify:
- WebUI loads at http://ip/
- Sidebar navigation works
- Dashboard shows stats
- Devices list shows connected USB devices
- Device detail panel expands with descriptor info
- Topology SVG renders correctly
- Settings forms load and save
- Auth can be enabled/disabled
- Protocol rejects v1.0.0 clients

- [ ] **Step 4: Commit final integration**
