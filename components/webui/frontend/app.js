// USB/IP Server WebUI - Application Logic
// Sections: Dashboard, Devices, Topology, Settings
(function() {
    'use strict';

    /* ================================================================
     * Utility functions
     * ================================================================ */

    function formatBytes(b) {
        if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB/s';
        if (b >= 1024) return (b / 1024).toFixed(1) + ' KB/s';
        return Math.round(b) + ' B/s';
    }

    function formatUptime(sec) {
        if (sec < 0 || !isFinite(sec)) return '--';
        var d = Math.floor(sec / 86400);
        var h = Math.floor((sec % 86400) / 3600);
        var m = Math.floor((sec % 3600) / 60);
        if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
        if (h > 0) return h + 'h ' + m + 'm';
        return m + 'm';
    }

    function hex4(v) {
        return ('0000' + v.toString(16)).slice(-4);
    }

    function esc(s) {
        if (!s) return '';
        var d = document.createElement('div');
        d.appendChild(document.createTextNode(s));
        return d.innerHTML;
    }

    function ip4str(ip) {
        if (!ip) return '';
        return (ip & 0xFF) + '.' +
               ((ip >>> 8) & 0xFF) + '.' +
               ((ip >>> 16) & 0xFF) + '.' +
               ((ip >>> 24) & 0xFF);
    }

    function speedStr(s) {
        return ['LS', 'FS', 'HS', '', '', 'SS'][s] || '?';
    }

    function speedClass(s) {
        return ['ls', 'fs', 'hs', '', '', 'ss'][s] || '';
    }

    function stateStr(s) {
        return ['Available', 'Exported', 'Error'][s] || 'Unknown';
    }

    function stateClass(s) {
        return ['available', 'exported', 'error'][s] || 'available';
    }

    var USB_CLASS = {
        0x00: 'Composite', 0x01: 'Audio', 0x02: 'CDC', 0x03: 'HID',
        0x05: 'Physical', 0x06: 'Image', 0x07: 'Printer', 0x08: 'Mass Storage',
        0x09: 'Hub', 0x0A: 'CDC Data', 0x0B: 'Smart Card', 0x0D: 'Content Security',
        0x0E: 'Video', 0x0F: 'Healthcare', 0x10: 'Audio/Video', 0x11: 'Billboard',
        0x12: 'USB-C Bridge', 0xDC: 'Diagnostic', 0xE0: 'Wireless',
        0xEF: 'Miscellaneous', 0xFE: 'Application Specific', 0xFF: 'Vendor Specific'
    };

    var EP_TYPE = ['Control', 'Isochronous', 'Bulk', 'Interrupt'];

    /* ================================================================
     * Toast notification
     * ================================================================ */

    function toast(msg, type) {
        var el = document.createElement('div');
        el.className = 'toast' + (type ? ' ' + type : '');
        el.textContent = msg;
        document.body.appendChild(el);
        /* Trigger reflow for transition */
        el.offsetHeight;
        el.classList.add('show');
        setTimeout(function() {
            el.classList.remove('show');
            setTimeout(function() {
                if (el.parentNode) el.parentNode.removeChild(el);
            }, 300);
        }, 3000);
    }

    /* ================================================================
     * DOM references
     * ================================================================ */

    var $sidebar = document.getElementById('sidebar');
    var $hamburger = document.getElementById('hamburger');
    var $navItems = document.querySelectorAll('.nav-item[data-section]');
    var $sections = document.querySelectorAll('.section');
    var $wsDot = document.getElementById('ws-dot');
    var $wsText = document.getElementById('ws-status-text');
    var $sidebarUptime = document.getElementById('sidebar-uptime');

    /* Dashboard */
    var $sDevices = document.getElementById('s-devices');
    var $sClients = document.getElementById('s-clients');
    var $sThroughput = document.getElementById('s-throughput');
    var $sMemory = document.getElementById('s-memory');
    var $sUptime = document.getElementById('s-uptime');
    var $sMemBar = document.getElementById('s-mem-bar');
    var $bwIn = document.getElementById('bw-in');
    var $bwOut = document.getElementById('bw-out');
    var $bwChart = document.getElementById('bw-chart');
    var $logArea = document.getElementById('log-area');

    /* Devices */
    var $deviceList = document.getElementById('device-list');

    /* Topology */
    var $topologySvg = document.getElementById('topology-svg');

    /* ================================================================
     * State
     * ================================================================ */

    var activeSection = 'dashboard';
    var ws = null;
    var wsReconnectTimer = null;

    /* Previous device bytes for throughput calc */
    var prevDevBytes = {};     /* idx -> { bi, bo, ts } */
    var devThroughput = {};    /* idx -> { bIn, bOut } */

    /* Global throughput */
    var prevTotalBytes = null; /* { bi, bo, ts } */
    var totalThroughputIn = 0;
    var totalThroughputOut = 0;

    /* Bandwidth chart rolling data */
    var BW_POINTS = 120;
    var bwIn = new Array(BW_POINTS);
    var bwOut = new Array(BW_POINTS);
    for (var _i = 0; _i < BW_POINTS; _i++) { bwIn[_i] = 0; bwOut[_i] = 0; }

    /* Last known stats for cross-section use */
    var lastStats = null;

    /* Expanded device index */
    var expandedDevIdx = -1;

    /* Track which log entries we've seen (by timestamp) */
    var renderedLogKeys = {};
    var logEntries = [];
    var MAX_LOG_ENTRIES = 15;

    /* Settings ACL working list */
    var aclIps = [];

    /* ================================================================
     * Sidebar navigation
     * ================================================================ */

    function switchSection(name) {
        activeSection = name;
        for (var i = 0; i < $navItems.length; i++) {
            var item = $navItems[i];
            if (item.getAttribute('data-section') === name) {
                item.classList.add('active');
            } else {
                item.classList.remove('active');
            }
        }
        for (var j = 0; j < $sections.length; j++) {
            var sec = $sections[j];
            if (sec.id === 'sec-' + name) {
                sec.classList.add('active');
            } else {
                sec.classList.remove('active');
            }
        }
        /* Close mobile sidebar */
        $sidebar.classList.remove('open');
        var overlay = document.querySelector('.sidebar-overlay');
        if (overlay) overlay.classList.remove('active');

        /* Section-specific init */
        if (name === 'settings') {
            loadAllSettings();
        }
        if (name === 'topology' && lastStats) {
            buildTopology(lastStats.devices || []);
        }
    }

    for (var ni = 0; ni < $navItems.length; ni++) {
        (function(item) {
            item.addEventListener('click', function(e) {
                e.preventDefault();
                switchSection(item.getAttribute('data-section'));
            });
        })($navItems[ni]);
    }

    /* Hamburger toggle */
    if ($hamburger) {
        $hamburger.addEventListener('click', function() {
            var isOpen = $sidebar.classList.toggle('open');
            var overlay = document.querySelector('.sidebar-overlay');
            if (!overlay) {
                overlay = document.createElement('div');
                overlay.className = 'sidebar-overlay';
                overlay.addEventListener('click', function() {
                    $sidebar.classList.remove('open');
                    overlay.classList.remove('active');
                });
                document.body.appendChild(overlay);
            }
            if (isOpen) {
                overlay.classList.add('active');
            } else {
                overlay.classList.remove('active');
            }
        });
    }

    /* ================================================================
     * WebSocket connection
     * ================================================================ */

    function wsConnect() {
        if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) {
            return;
        }
        var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        ws = new WebSocket(proto + '//' + location.host + '/ws');

        ws.onopen = function() {
            $wsDot.classList.add('connected');
            $wsText.textContent = 'Connected';
            if (wsReconnectTimer) {
                clearTimeout(wsReconnectTimer);
                wsReconnectTimer = null;
            }
        };

        ws.onclose = function() {
            $wsDot.classList.remove('connected');
            $wsText.textContent = 'Disconnected';
            ws = null;
            if (!wsReconnectTimer) {
                wsReconnectTimer = setTimeout(function() {
                    wsReconnectTimer = null;
                    wsConnect();
                }, 2000);
            }
        };

        ws.onerror = function() {
            /* onclose will fire after onerror */
        };

        ws.onmessage = function(ev) {
            try {
                var msg = JSON.parse(ev.data);
                if (msg.type === 'stats') {
                    handleStats(msg);
                }
            } catch (e) {
                /* ignore malformed messages */
            }
        };
    }

    /* ================================================================
     * Stats handler - dispatches to all sections
     * ================================================================ */

    function handleStats(msg) {
        lastStats = msg;
        var devices = msg.devices || [];
        var now = Date.now();

        /* --- Compute per-device throughput --- */
        var totalBi = 0, totalBo = 0;
        for (var i = 0; i < devices.length; i++) {
            var d = devices[i];
            totalBi += d.bi;
            totalBo += d.bo;
            var prev = prevDevBytes[d.idx];
            if (prev) {
                var dt = (now - prev.ts) / 1000;
                if (dt > 0) {
                    devThroughput[d.idx] = {
                        bIn: Math.max(0, (d.bi - prev.bi) / dt),
                        bOut: Math.max(0, (d.bo - prev.bo) / dt)
                    };
                }
            }
            prevDevBytes[d.idx] = { bi: d.bi, bo: d.bo, ts: now };
        }

        /* --- Global throughput --- */
        if (prevTotalBytes) {
            var gdt = (now - prevTotalBytes.ts) / 1000;
            if (gdt > 0) {
                totalThroughputIn = Math.max(0, (totalBi - prevTotalBytes.bi) / gdt);
                totalThroughputOut = Math.max(0, (totalBo - prevTotalBytes.bo) / gdt);
            }
        }
        prevTotalBytes = { bi: totalBi, bo: totalBo, ts: now };

        /* Push bandwidth data */
        bwIn.push(totalThroughputIn);
        bwOut.push(totalThroughputOut);
        if (bwIn.length > BW_POINTS) bwIn.shift();
        if (bwOut.length > BW_POINTS) bwOut.shift();

        /* --- Count unique clients --- */
        var clientSet = {};
        var clientCount = 0;
        for (var ci = 0; ci < devices.length; ci++) {
            var cip = devices[ci].client_ip;
            if (cip && !clientSet[cip]) {
                clientSet[cip] = true;
                clientCount++;
            }
        }

        /* --- Update dashboard --- */
        updateDashboard(msg, devices, clientCount);

        /* --- Update devices section --- */
        if (activeSection === 'devices') {
            updateDeviceList(devices);
        }

        /* --- Update topology section --- */
        if (activeSection === 'topology') {
            buildTopology(devices);
        }

        /* --- Logs --- */
        if (msg.logs) {
            updateLogs(msg.logs);
        }
    }

    /* ================================================================
     * Dashboard
     * ================================================================ */

    /* Estimate total internal heap ~400KB for ESP32-P4 */
    var TOTAL_INTERNAL_HEAP = 400 * 1024;

    function updateDashboard(msg, devices, clientCount) {
        $sDevices.textContent = devices.length;
        $sClients.textContent = clientCount;
        $sThroughput.textContent = formatBytes(totalThroughputIn + totalThroughputOut);
        $sUptime.textContent = formatUptime(msg.uptime_sec);
        $sidebarUptime.textContent = 'Uptime: ' + formatUptime(msg.uptime_sec);

        /* Memory */
        var usedInternal = TOTAL_INTERNAL_HEAP - msg.free_internal;
        var pct = Math.min(100, Math.max(0, (usedInternal / TOTAL_INTERNAL_HEAP) * 100));
        $sMemory.textContent = Math.round(msg.free_heap / 1024) + ' KB';
        $sMemBar.style.width = pct.toFixed(1) + '%';

        /* Bandwidth summary */
        $bwIn.textContent = formatBytes(totalThroughputIn);
        $bwOut.textContent = formatBytes(totalThroughputOut);

        /* Chart */
        drawBandwidthChart();
    }

    /* ================================================================
     * Bandwidth chart
     * ================================================================ */

    function drawBandwidthChart() {
        var canvas = $bwChart;
        if (!canvas) return;

        /* Handle high-DPI displays */
        var rect = canvas.getBoundingClientRect();
        var w = rect.width;
        var h = rect.height;
        if (w === 0 || h === 0) return;

        var dpr = window.devicePixelRatio || 1;
        canvas.width = w * dpr;
        canvas.height = h * dpr;

        var ctx = canvas.getContext('2d');
        ctx.scale(dpr, dpr);
        ctx.clearRect(0, 0, w, h);

        /* Find max value for Y axis */
        var maxVal = 1024; /* minimum 1 KB/s scale */
        for (var i = 0; i < BW_POINTS; i++) {
            if (bwIn[i] > maxVal) maxVal = bwIn[i];
            if (bwOut[i] > maxVal) maxVal = bwOut[i];
        }
        maxVal *= 1.15; /* headroom */

        var padL = 0, padR = 0, padT = 8, padB = 4;
        var cw = w - padL - padR;
        var ch = h - padT - padB;

        /* Grid lines */
        ctx.strokeStyle = '#e8e8ef';
        ctx.lineWidth = 0.5;
        var gridLines = 4;
        for (var g = 0; g <= gridLines; g++) {
            var gy = padT + (ch * g / gridLines);
            ctx.beginPath();
            ctx.moveTo(padL, gy);
            ctx.lineTo(padL + cw, gy);
            ctx.stroke();
        }

        /* Draw a filled line */
        function drawLine(data, strokeColor, fillColor) {
            var step = cw / (BW_POINTS - 1);
            ctx.beginPath();
            for (var j = 0; j < BW_POINTS; j++) {
                var x = padL + j * step;
                var y = padT + ch - (data[j] / maxVal) * ch;
                if (j === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            ctx.strokeStyle = strokeColor;
            ctx.lineWidth = 1.5;
            ctx.stroke();

            /* Fill area under */
            ctx.lineTo(padL + (BW_POINTS - 1) * step, padT + ch);
            ctx.lineTo(padL, padT + ch);
            ctx.closePath();
            ctx.fillStyle = fillColor;
            ctx.fill();
        }

        drawLine(bwIn, '#3b82f6', 'rgba(59,130,246,0.10)');
        drawLine(bwOut, '#22c55e', 'rgba(34,197,94,0.08)');
    }

    /* ================================================================
     * Event log
     * ================================================================ */

    var LOG_LEVEL = ['DEBUG', 'INFO', 'WARN', 'ERROR'];
    var LOG_CLASS = ['debug', 'info', 'warn', 'error'];

    function updateLogs(logs) {
        if (!logs || !logs.length) return;

        /* Merge new entries */
        var changed = false;
        for (var i = 0; i < logs.length; i++) {
            var entry = logs[i];
            var key = entry.ts + ':' + entry.msg;
            if (!renderedLogKeys[key]) {
                renderedLogKeys[key] = true;
                logEntries.push(entry);
                changed = true;
            }
        }

        if (!changed) return;

        /* Sort by timestamp descending, keep last N */
        logEntries.sort(function(a, b) { return b.ts - a.ts; });
        if (logEntries.length > MAX_LOG_ENTRIES) {
            var removed = logEntries.splice(MAX_LOG_ENTRIES);
            for (var r = 0; r < removed.length; r++) {
                delete renderedLogKeys[removed[r].ts + ':' + removed[r].msg];
            }
        }

        /* Render */
        var html = '';
        for (var j = 0; j < logEntries.length; j++) {
            var e = logEntries[j];
            var lvl = e.level || 1;
            var cls = LOG_CLASS[lvl] || 'info';
            /* timestamp: convert microseconds to readable time */
            var sec = Math.floor(e.ts / 1000000);
            var tsStr = formatUptime(sec);
            html += '<div class="log-entry ' + cls + '">' +
                    '<span class="ts">' + esc(tsStr) + '</span>' +
                    '<span class="msg">' + esc(e.msg) + '</span></div>';
        }
        $logArea.innerHTML = html;
    }

    /* ================================================================
     * Devices section
     * ================================================================ */

    function updateDeviceList(devices) {
        if (!devices.length) {
            $deviceList.innerHTML = '<div class="empty-state">No USB devices connected</div>';
            return;
        }

        /* Build or update device cards - reconcile existing DOM */
        var existingCards = $deviceList.querySelectorAll('.device-card');
        var existingMap = {};
        for (var e = 0; e < existingCards.length; e++) {
            var eidx = existingCards[e].getAttribute('data-idx');
            existingMap[eidx] = existingCards[e];
        }

        /* Track which indices are present */
        var presentIdxs = {};

        for (var i = 0; i < devices.length; i++) {
            var d = devices[i];
            presentIdxs[d.idx] = true;
            var tp = devThroughput[d.idx] || { bIn: 0, bOut: 0 };
            var isExpanded = (expandedDevIdx === d.idx);
            var spdStr = speedStr(d.speed);
            var spdCls = speedClass(d.speed);
            var stCls = stateClass(d.state);
            var stStr = stateStr(d.state);
            var devName = d.prod || d.mfr || ('Device ' + hex4(d.vid) + ':' + hex4(d.pid));
            var clientStr = d.client_ip ? ip4str(d.client_ip) : '';

            /* Determine primary class from icl */
            var primaryClass = '';
            if (d.icl && d.icl.length > 0) {
                for (var ic = 0; ic < d.icl.length; ic++) {
                    if (d.icl[ic] !== 0) {
                        primaryClass = USB_CLASS[d.icl[ic]] || ('Class 0x' + hex4(d.icl[ic]));
                        break;
                    }
                }
                if (!primaryClass && d.icl.length > 0) {
                    primaryClass = USB_CLASS[d.icl[0]] || '';
                }
            }

            var card = existingMap[d.idx];
            if (card) {
                /* Update existing card in place */
                var dot = card.querySelector('.status-dot');
                if (dot) dot.className = 'status-dot ' + stCls;

                var nameEl = card.querySelector('.device-name');
                if (nameEl) nameEl.textContent = devName;

                var idsEl = card.querySelector('.device-ids');
                if (idsEl) {
                    var idsText = d.path + '  ' + hex4(d.vid) + ':' + hex4(d.pid);
                    if (primaryClass) idsText += '  ' + primaryClass;
                    idsEl.textContent = idsText;
                }

                /* Update badges */
                var badgesEl = card.querySelector('.device-badges');
                if (badgesEl) {
                    var badgeHtml = '<span class="badge badge-' + spdCls + '">' + spdStr + '</span>' +
                                   '<span class="badge badge-state badge-' + stCls + '">' + stStr + '</span>';
                    if (clientStr) {
                        badgeHtml += '<span class="badge badge-state">' + esc(clientStr) + '</span>';
                    }
                    var tpTotal = tp.bIn + tp.bOut;
                    if (tpTotal > 0) {
                        badgeHtml += '<span class="badge badge-state">' + formatBytes(tpTotal) + '</span>';
                    }
                    badgesEl.innerHTML = badgeHtml;
                }

                delete existingMap[d.idx];
            } else {
                /* Create new card */
                card = document.createElement('div');
                card.className = 'device-card' + (isExpanded ? ' expanded' : '');
                card.setAttribute('data-idx', d.idx);

                var idsText = d.path + '  ' + hex4(d.vid) + ':' + hex4(d.pid);
                if (primaryClass) idsText += '  ' + primaryClass;

                var badgeHtml = '<span class="badge badge-' + spdCls + '">' + spdStr + '</span>' +
                               '<span class="badge badge-state badge-' + stCls + '">' + stStr + '</span>';
                if (clientStr) {
                    badgeHtml += '<span class="badge badge-state">' + esc(clientStr) + '</span>';
                }
                var tpTotal = tp.bIn + tp.bOut;
                if (tpTotal > 0) {
                    badgeHtml += '<span class="badge badge-state">' + formatBytes(tpTotal) + '</span>';
                }

                card.innerHTML =
                    '<div class="device-card-header" onclick="window._toggleDevice(' + d.idx + ')">' +
                        '<div class="status-dot ' + stCls + '"></div>' +
                        '<div class="device-summary">' +
                            '<div class="device-name">' + esc(devName) + '</div>' +
                            '<div class="device-ids">' + esc(idsText) + '</div>' +
                        '</div>' +
                        '<div class="device-badges">' + badgeHtml + '</div>' +
                        '<div class="expand-icon">&#9660;</div>' +
                    '</div>' +
                    '<div class="device-card-body" id="dev-body-' + d.idx + '">' +
                        '<div class="text-dim" style="padding:1rem 0;text-align:center">Loading...</div>' +
                    '</div>';

                $deviceList.appendChild(card);
            }
        }

        /* Remove cards for devices no longer present */
        for (var oldIdx in existingMap) {
            if (existingMap.hasOwnProperty(oldIdx)) {
                var old = existingMap[oldIdx];
                if (old.parentNode) old.parentNode.removeChild(old);
            }
        }
    }

    /* Toggle device detail expansion */
    window._toggleDevice = function(idx) {
        var card = $deviceList.querySelector('.device-card[data-idx="' + idx + '"]');
        if (!card) return;

        if (expandedDevIdx === idx) {
            /* Collapse */
            card.classList.remove('expanded');
            expandedDevIdx = -1;
            return;
        }

        /* Collapse previous */
        var prev = $deviceList.querySelector('.device-card.expanded');
        if (prev) prev.classList.remove('expanded');

        expandedDevIdx = idx;
        card.classList.add('expanded');

        /* Fetch detail */
        fetchDeviceDetail(idx);
    };

    function fetchDeviceDetail(idx) {
        var body = document.getElementById('dev-body-' + idx);
        if (!body) return;
        body.innerHTML = '<div class="text-dim" style="padding:1rem 0;text-align:center">Loading...</div>';

        fetch('/api/devices/detail?idx=' + idx)
            .then(function(r) { return r.json(); })
            .then(function(detail) {
                renderDeviceDetail(idx, detail);
            })
            .catch(function(err) {
                body.innerHTML = '<div class="text-dim" style="padding:1rem 0;text-align:center">Failed to load details</div>';
            });
    }

    function renderDeviceDetail(idx, d) {
        var body = document.getElementById('dev-body-' + idx);
        if (!body) return;

        var html = '';

        /* Device info rows */
        html += '<div class="device-detail-row"><span class="detail-label">Bus ID</span><span class="detail-value">' + esc(d.path) + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">VID:PID</span><span class="detail-value">' + hex4(d.vid) + ':' + hex4(d.pid) + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">BCD Device</span><span class="detail-value">' + hex4(d.bcd_device || 0) + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">Class</span><span class="detail-value">' + (USB_CLASS[d['class']] || ('0x' + hex4(d['class'] || 0))) + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">Speed</span><span class="detail-value">' + esc(d.speed) + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">State</span><span class="detail-value">' + esc(d.state) + '</span></div>';
        if (d.client_ip) {
            html += '<div class="device-detail-row"><span class="detail-label">Client</span><span class="detail-value">' + esc(d.client_ip) + '</span></div>';
        }
        html += '<div class="device-detail-row"><span class="detail-label">Manufacturer</span><span class="detail-value">' + esc(d.manufacturer || '--') + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">Product</span><span class="detail-value">' + esc(d.product || '--') + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">Serial</span><span class="detail-value">' + esc(d.serial || '--') + '</span></div>';

        /* Stats */
        html += '<div class="device-detail-row"><span class="detail-label">Bytes In</span><span class="detail-value">' + (d.bytes_in || 0).toLocaleString() + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">Bytes Out</span><span class="detail-value">' + (d.bytes_out || 0).toLocaleString() + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">URBs OK</span><span class="detail-value">' + (d.urbs_completed || 0).toLocaleString() + '</span></div>';
        html += '<div class="device-detail-row"><span class="detail-label">URBs Failed</span><span class="detail-value">' + (d.urbs_failed || 0).toLocaleString() + '</span></div>';

        /* Interfaces table */
        if (d.interfaces && d.interfaces.length) {
            html += '<div style="margin-top:0.75rem;font-size:0.8125rem;font-weight:600;color:var(--text-secondary);text-transform:uppercase;letter-spacing:0.04em">Interfaces</div>';
            html += '<table style="width:100%;font-size:0.8125rem;border-collapse:collapse;margin-top:0.375rem">';
            html += '<tr style="color:var(--text-dim);text-align:left"><th style="padding:0.25rem 0.5rem;font-weight:500">#</th><th style="padding:0.25rem 0.5rem;font-weight:500">Class</th><th style="padding:0.25rem 0.5rem;font-weight:500">Sub</th><th style="padding:0.25rem 0.5rem;font-weight:500">Proto</th><th style="padding:0.25rem 0.5rem;font-weight:500">EPs</th></tr>';
            for (var ii = 0; ii < d.interfaces.length; ii++) {
                var iface = d.interfaces[ii];
                var cls = USB_CLASS[iface['class']] || ('0x' + ('00' + iface['class'].toString(16)).slice(-2));
                html += '<tr style="border-top:1px solid var(--border-light)">' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + iface.number + '</td>' +
                    '<td style="padding:0.25rem 0.5rem">' + esc(cls) + '</td>' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + iface.subclass + '</td>' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + iface.protocol + '</td>' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + iface.num_endpoints + '</td>' +
                    '</tr>';
            }
            html += '</table>';
        }

        /* Endpoints table */
        if (d.endpoints && d.endpoints.length) {
            html += '<div style="margin-top:0.75rem;font-size:0.8125rem;font-weight:600;color:var(--text-secondary);text-transform:uppercase;letter-spacing:0.04em">Endpoints</div>';
            html += '<table style="width:100%;font-size:0.8125rem;border-collapse:collapse;margin-top:0.375rem">';
            html += '<tr style="color:var(--text-dim);text-align:left"><th style="padding:0.25rem 0.5rem;font-weight:500">Addr</th><th style="padding:0.25rem 0.5rem;font-weight:500">Dir</th><th style="padding:0.25rem 0.5rem;font-weight:500">Type</th><th style="padding:0.25rem 0.5rem;font-weight:500">MaxPkt</th><th style="padding:0.25rem 0.5rem;font-weight:500">Interval</th></tr>';
            for (var ei = 0; ei < d.endpoints.length; ei++) {
                var ep = d.endpoints[ei];
                html += '<tr style="border-top:1px solid var(--border-light)">' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + esc(ep.address) + '</td>' +
                    '<td style="padding:0.25rem 0.5rem">' + esc(ep.direction) + '</td>' +
                    '<td style="padding:0.25rem 0.5rem">' + esc(ep.type) + '</td>' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + ep.max_packet_size + '</td>' +
                    '<td style="padding:0.25rem 0.5rem;font-family:var(--mono)">' + ep.interval + '</td>' +
                    '</tr>';
            }
            html += '</table>';
        }

        /* Action buttons */
        html += '<div class="device-card-actions">';
        if (d.state === 'Exported') {
            html += '<button class="btn btn-sm btn-secondary" onclick="window._deviceAction(\'disconnect\',' + idx + ')">Disconnect Client</button>';
        }
        html += '<button class="btn btn-sm btn-secondary" onclick="window._deviceAction(\'export\',' + idx + ')">Toggle Export</button>';
        html += '<button class="btn btn-sm btn-warn" onclick="window._deviceAction(\'reset\',' + idx + ')">Reset</button>';
        html += '</div>';

        body.innerHTML = html;
    }

    /* Device action handler */
    window._deviceAction = function(action, idx) {
        var url = '/api/devices/' + action + '?idx=' + idx;
        fetch(url, { method: 'POST' })
            .then(function(r) { return r.json(); })
            .then(function(data) {
                if (data.ok) {
                    toast('Action completed', 'success');
                    /* Re-fetch detail if still expanded */
                    if (expandedDevIdx === idx) {
                        setTimeout(function() { fetchDeviceDetail(idx); }, 300);
                    }
                } else {
                    toast(data.error || 'Action failed', 'error');
                }
            })
            .catch(function() {
                toast('Request failed', 'error');
            });
    };

    /* Navigate to device from topology */
    window._showDevice = function(idx) {
        switchSection('devices');
        expandedDevIdx = -1; /* reset so toggle will expand */
        /* Wait for device list to render, then toggle */
        setTimeout(function() {
            window._toggleDevice(idx);
        }, 100);
    };

    /* ================================================================
     * Topology section - SVG tree
     * ================================================================ */

    function buildTopology(devices) {
        if (!devices || !devices.length) {
            $topologySvg.innerHTML = '<svg viewBox="0 0 600 120"><text x="300" y="60" text-anchor="middle" fill="#94a3b8" font-family="system-ui" font-size="14">No devices connected</text></svg>';
            return;
        }

        /* Parse hierarchy from busid (path field) */
        /* "1-1" = root device, "1-1.3" = behind hub port 3, "1-1.3.2" = deeper */
        var rootDevices = [];   /* direct on root hub */
        var hubChildren = {};   /* hubIdx -> [{ port, device }] */
        var hubIndices = {};    /* device idx -> true if it's a hub */

        /* Detect hubs */
        for (var h = 0; h < devices.length; h++) {
            var dh = devices[h];
            if (dh.icl) {
                for (var hc = 0; hc < dh.icl.length; hc++) {
                    if (dh.icl[hc] === 0x09) {
                        hubIndices[dh.idx] = true;
                        break;
                    }
                }
            }
        }

        /* Sort devices by path for consistent layout */
        var sorted = devices.slice().sort(function(a, b) {
            return a.path < b.path ? -1 : a.path > b.path ? 1 : 0;
        });

        /* Build parent map: for "1-1.3", parent path is "1-1" */
        var devByPath = {};
        for (var dp = 0; dp < sorted.length; dp++) {
            devByPath[sorted[dp].path] = sorted[dp];
        }

        for (var s = 0; s < sorted.length; s++) {
            var dev = sorted[s];
            var parts = dev.path.split('.');
            if (parts.length <= 1) {
                /* Root-level device (e.g. "1-1") */
                rootDevices.push(dev);
            } else {
                /* Child device - find parent hub */
                var parentPath = parts.slice(0, -1).join('.');
                var port = parseInt(parts[parts.length - 1], 10);
                var parentDev = devByPath[parentPath];
                if (parentDev) {
                    if (!hubChildren[parentDev.idx]) hubChildren[parentDev.idx] = [];
                    hubChildren[parentDev.idx].push({ port: port, device: dev });
                } else {
                    /* Parent not found, treat as root */
                    rootDevices.push(dev);
                }
            }
        }

        /* Layout constants */
        var nodeW = 150, nodeH = 52;
        var hubNodeH = 70;
        var rootY = 20, tier1Y = 110, tier2Y = 220;
        var hGap = 20;
        var rootNodeW = 190, rootNodeH = 40;

        /* Calculate total width needed */
        var columns = [];
        for (var ri = 0; ri < rootDevices.length; ri++) {
            var rd = rootDevices[ri];
            var children = hubChildren[rd.idx] || [];
            if (hubIndices[rd.idx] && children.length > 0) {
                /* Hub needs width for all children */
                var hubW = Math.max(nodeW, children.length * (nodeW + hGap) - hGap);
                columns.push({ dev: rd, width: hubW, children: children, isHub: true });
            } else {
                columns.push({ dev: rd, width: nodeW, children: [], isHub: false });
            }
        }

        var totalWidth = 0;
        for (var tw = 0; tw < columns.length; tw++) {
            totalWidth += columns[tw].width;
            if (tw > 0) totalWidth += hGap * 2;
        }
        totalWidth = Math.max(totalWidth, rootNodeW) + 80; /* padding */

        var hasChildren = false;
        for (var hcc = 0; hcc < columns.length; hcc++) {
            if (columns[hcc].children.length > 0) { hasChildren = true; break; }
        }
        var svgH = hasChildren ? tier2Y + nodeH + 30 : tier1Y + nodeH + 30;

        var svg = '<svg viewBox="0 0 ' + totalWidth + ' ' + svgH + '" xmlns="http://www.w3.org/2000/svg">';

        /* Root controller node */
        var rootX = totalWidth / 2;
        svg += '<g class="topo-node">';
        svg += '<rect x="' + (rootX - rootNodeW / 2) + '" y="' + rootY + '" width="' + rootNodeW + '" height="' + rootNodeH + '" rx="8" fill="#fff" stroke="#3b82f6" stroke-width="2"/>';
        svg += '<text x="' + rootX + '" y="' + (rootY + 24) + '" text-anchor="middle" font-family="system-ui" font-size="12" font-weight="600" fill="#1a202c">ESP32-P4 Host Controller</text>';
        svg += '</g>';

        /* Place tier 1 columns */
        var startX = (totalWidth - (totalWidth - 80)) / 2;
        var xCursor = startX;

        for (var ci = 0; ci < columns.length; ci++) {
            var col = columns[ci];
            var colCenter = xCursor + col.width / 2;
            var d = col.dev;
            var borderColor = d.state === 1 ? '#22c55e' : d.state === 0 ? '#3b82f6' : '#94a3b8';
            var nh = col.isHub ? hubNodeH : nodeH;
            var devLabel = d.prod || d.mfr || hex4(d.vid) + ':' + hex4(d.pid);
            if (devLabel.length > 18) devLabel = devLabel.substring(0, 16) + '..';

            /* Line from root to device */
            var rootBottom = rootY + rootNodeH;
            svg += '<g class="topo-edge">';
            svg += '<line x1="' + rootX + '" y1="' + rootBottom + '" x2="' + rootX + '" y2="' + (rootBottom + 15) + '" stroke="#cbd5e1" stroke-width="1.5"/>';
            svg += '<line x1="' + rootX + '" y1="' + (rootBottom + 15) + '" x2="' + colCenter + '" y2="' + (rootBottom + 15) + '" stroke="#cbd5e1" stroke-width="1.5"/>';
            svg += '<line x1="' + colCenter + '" y1="' + (rootBottom + 15) + '" x2="' + colCenter + '" y2="' + tier1Y + '" stroke="#cbd5e1" stroke-width="1.5"/>';
            svg += '</g>';

            /* Device node */
            var nodeClass = col.isHub ? 'topo-hub' : 'topo-node';
            svg += '<g class="' + nodeClass + '" style="cursor:pointer" onclick="window._showDevice(' + d.idx + ')">';
            svg += '<rect x="' + (colCenter - nodeW / 2) + '" y="' + tier1Y + '" width="' + nodeW + '" height="' + nh + '" rx="8" fill="' + (col.isHub ? 'rgba(59,130,246,0.06)' : '#fff') + '" stroke="' + borderColor + '" stroke-width="1.5"/>';
            svg += '<text x="' + colCenter + '" y="' + (tier1Y + 20) + '" text-anchor="middle" font-family="system-ui" font-size="11" font-weight="600" fill="#1a202c">' + esc(d.path) + '</text>';
            svg += '<text x="' + colCenter + '" y="' + (tier1Y + 35) + '" text-anchor="middle" font-family="system-ui" font-size="10" fill="#64748b">' + esc(devLabel) + '</text>';

            /* Speed badge in SVG */
            var spdTxt = speedStr(d.speed);
            var badgeCols = { 'HS': '#22c55e', 'FS': '#3b82f6', 'LS': '#f59e0b', 'SS': '#9333ea' };
            var badgeCol = badgeCols[spdTxt] || '#94a3b8';
            svg += '<text x="' + (colCenter + nodeW / 2 - 10) + '" y="' + (tier1Y + 14) + '" text-anchor="end" font-family="system-ui" font-size="9" font-weight="700" fill="' + badgeCol + '">' + spdTxt + '</text>';

            if (col.isHub && col.children.length > 0) {
                /* Draw port circles along bottom of hub */
                var portY = tier1Y + nh - 10;
                var portSpacing = Math.min(20, (nodeW - 20) / Math.max(col.children.length, 1));
                var portStart = colCenter - ((col.children.length - 1) * portSpacing) / 2;
                for (var pi = 0; pi < col.children.length; pi++) {
                    var px = portStart + pi * portSpacing;
                    svg += '<circle cx="' + px + '" cy="' + portY + '" r="4" fill="#e2e8f0" stroke="#94a3b8" stroke-width="1"/>';
                    svg += '<text x="' + px + '" y="' + (portY + 3) + '" text-anchor="middle" font-family="system-ui" font-size="6" fill="#64748b">' + col.children[pi].port + '</text>';
                }
            }

            svg += '</g>';

            /* Draw children (tier 2) */
            if (col.children.length > 0) {
                var childStartX = colCenter - ((col.children.length * (nodeW + hGap)) - hGap) / 2;
                for (var ki = 0; ki < col.children.length; ki++) {
                    var child = col.children[ki].device;
                    var cx = childStartX + ki * (nodeW + hGap) + nodeW / 2;
                    var childBorder = child.state === 1 ? '#22c55e' : child.state === 0 ? '#3b82f6' : '#94a3b8';
                    var childLabel = child.prod || child.mfr || hex4(child.vid) + ':' + hex4(child.pid);
                    if (childLabel.length > 18) childLabel = childLabel.substring(0, 16) + '..';

                    /* Connecting line */
                    var hubBottom = tier1Y + nh;
                    svg += '<g class="topo-edge">';
                    svg += '<line x1="' + colCenter + '" y1="' + hubBottom + '" x2="' + colCenter + '" y2="' + (hubBottom + 15) + '" stroke="#cbd5e1" stroke-width="1.5"/>';
                    svg += '<line x1="' + colCenter + '" y1="' + (hubBottom + 15) + '" x2="' + cx + '" y2="' + (hubBottom + 15) + '" stroke="#cbd5e1" stroke-width="1.5"/>';
                    svg += '<line x1="' + cx + '" y1="' + (hubBottom + 15) + '" x2="' + cx + '" y2="' + tier2Y + '" stroke="#cbd5e1" stroke-width="1.5"/>';
                    svg += '</g>';

                    /* Child device node */
                    svg += '<g class="topo-node" style="cursor:pointer" onclick="window._showDevice(' + child.idx + ')">';
                    svg += '<rect x="' + (cx - nodeW / 2) + '" y="' + tier2Y + '" width="' + nodeW + '" height="' + nodeH + '" rx="8" fill="#fff" stroke="' + childBorder + '" stroke-width="1.5"/>';
                    svg += '<text x="' + cx + '" y="' + (tier2Y + 20) + '" text-anchor="middle" font-family="system-ui" font-size="11" font-weight="600" fill="#1a202c">' + esc(child.path) + '</text>';
                    svg += '<text x="' + cx + '" y="' + (tier2Y + 35) + '" text-anchor="middle" font-family="system-ui" font-size="10" fill="#64748b">' + esc(childLabel) + '</text>';

                    var cSpdTxt = speedStr(child.speed);
                    var cBadgeCol = badgeCols[cSpdTxt] || '#94a3b8';
                    svg += '<text x="' + (cx + nodeW / 2 - 10) + '" y="' + (tier2Y + 14) + '" text-anchor="end" font-family="system-ui" font-size="9" font-weight="700" fill="' + cBadgeCol + '">' + cSpdTxt + '</text>';

                    svg += '</g>';
                }
            }

            xCursor += col.width + hGap * 2;
        }

        svg += '</svg>';
        $topologySvg.innerHTML = svg;
    }

    /* ================================================================
     * Settings section
     * ================================================================ */

    function loadAllSettings() {
        loadNetworkSettings();
        loadAuthSettings();
        loadAclSettings();
        loadSystemInfo();
    }

    /* --- Network --- */

    var $setHostname = document.getElementById('set-hostname');
    var $setDhcp = document.getElementById('set-dhcp');
    var $staticFields = document.getElementById('static-fields');
    var $setIp = document.getElementById('set-ip');
    var $setMask = document.getElementById('set-mask');
    var $setGw = document.getElementById('set-gw');
    var $setDns = document.getElementById('set-dns');
    var $setCurrentIp = document.getElementById('set-current-ip');

    if ($setDhcp) {
        $setDhcp.addEventListener('change', function() {
            $staticFields.style.display = this.checked ? 'none' : 'block';
        });
    }

    function loadNetworkSettings() {
        fetch('/api/settings/network')
            .then(function(r) { return r.json(); })
            .then(function(data) {
                if ($setHostname) $setHostname.value = data.hostname || '';
                if ($setDhcp) {
                    $setDhcp.checked = (data.dhcp !== false && data.dhcp !== 'false');
                    $staticFields.style.display = $setDhcp.checked ? 'none' : 'block';
                }
                if ($setIp) $setIp.value = data.static_ip || '';
                if ($setMask) $setMask.value = data.netmask || '';
                if ($setGw) $setGw.value = data.gateway || '';
                if ($setDns) $setDns.value = data.dns || '';
            })
            .catch(function() { /* ignore */ });

        /* Get current IP from system info */
        fetch('/api/stats')
            .then(function(r) { return r.json(); })
            .then(function(data) {
                if ($setCurrentIp && data.ip) $setCurrentIp.textContent = data.ip;
            })
            .catch(function() { /* ignore */ });
    }

    window.saveNetwork = function() {
        var body = {
            hostname: $setHostname.value,
            dhcp: $setDhcp.checked ? 'true' : 'false',
            static_ip: $setIp.value,
            netmask: $setMask.value,
            gateway: $setGw.value,
            dns: $setDns.value
        };
        fetch('/api/settings/network', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.ok) {
                toast('Network settings saved. Reboot to apply.', 'success');
            } else {
                toast(data.error || 'Save failed', 'error');
            }
        })
        .catch(function() { toast('Request failed', 'error'); });
    };

    /* --- Auth --- */

    var $setAuthEnabled = document.getElementById('set-auth-enabled');
    var $authFields = document.getElementById('auth-fields');
    var $setAuthUser = document.getElementById('set-auth-user');
    var $setAuthPass = document.getElementById('set-auth-pass');

    if ($setAuthEnabled) {
        $setAuthEnabled.addEventListener('change', function() {
            $authFields.style.display = this.checked ? 'block' : 'none';
        });
    }

    function loadAuthSettings() {
        fetch('/api/settings/auth')
            .then(function(r) { return r.json(); })
            .then(function(data) {
                if ($setAuthEnabled) {
                    $setAuthEnabled.checked = !!data.enabled;
                    $authFields.style.display = data.enabled ? 'block' : 'none';
                }
                if ($setAuthUser) $setAuthUser.value = data.username || '';
                if ($setAuthPass) $setAuthPass.value = '';
            })
            .catch(function() { /* ignore */ });
    }

    window.saveAuth = function() {
        var body = {
            enabled: $setAuthEnabled.checked,
            username: $setAuthUser.value,
            password: $setAuthPass.value
        };
        fetch('/api/settings/auth', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.ok) {
                toast('Auth settings saved', 'success');
            } else {
                toast(data.error || 'Save failed', 'error');
            }
        })
        .catch(function() { toast('Request failed', 'error'); });
    };

    /* --- ACL --- */

    var $aclRadios = document.querySelectorAll('input[name="acl-mode"]');
    var $aclListSection = document.getElementById('acl-list-section');
    var $aclList = document.getElementById('acl-list');
    var $aclNewIp = document.getElementById('acl-new-ip');

    for (var ai = 0; ai < $aclRadios.length; ai++) {
        $aclRadios[ai].addEventListener('change', function() {
            $aclListSection.style.display = this.value === 'closed' ? 'block' : 'none';
        });
    }

    function loadAclSettings() {
        fetch('/api/settings/acl')
            .then(function(r) { return r.json(); })
            .then(function(data) {
                /* Set mode radio */
                var mode = data.closed ? 'closed' : 'open';
                for (var i = 0; i < $aclRadios.length; i++) {
                    $aclRadios[i].checked = ($aclRadios[i].value === mode);
                }
                $aclListSection.style.display = data.closed ? 'block' : 'none';

                /* Set IP list */
                aclIps = data.allowlist || [];
                renderAclList();
            })
            .catch(function() { /* ignore */ });
    }

    function renderAclList() {
        if (!$aclList) return;
        if (!aclIps.length) {
            $aclList.innerHTML = '<div class="text-dim" style="padding:0.5rem 0;font-size:0.8125rem">No IPs in allowlist</div>';
            return;
        }
        var html = '';
        for (var i = 0; i < aclIps.length; i++) {
            html += '<div class="acl-item">' +
                '<span class="acl-ip">' + esc(aclIps[i]) + '</span>' +
                '<button onclick="window._removeAclIp(' + i + ')">Remove</button>' +
                '</div>';
        }
        $aclList.innerHTML = html;
    }

    window._removeAclIp = function(index) {
        aclIps.splice(index, 1);
        renderAclList();
    };

    window.addAclIp = function() {
        var ip = $aclNewIp.value.trim();
        if (!ip) return;
        /* Basic IP validation */
        if (!/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(ip)) {
            toast('Invalid IP address', 'error');
            return;
        }
        if (aclIps.indexOf(ip) !== -1) {
            toast('IP already in list', 'error');
            return;
        }
        aclIps.push(ip);
        $aclNewIp.value = '';
        renderAclList();
    };

    window.saveAcl = function() {
        var closed = false;
        for (var i = 0; i < $aclRadios.length; i++) {
            if ($aclRadios[i].checked && $aclRadios[i].value === 'closed') {
                closed = true;
                break;
            }
        }
        var body = {
            closed: closed,
            allowlist: aclIps
        };
        fetch('/api/settings/acl', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.ok) {
                toast('ACL settings saved', 'success');
            } else {
                toast(data.error || 'Save failed', 'error');
            }
        })
        .catch(function() { toast('Request failed', 'error'); });
    };

    /* --- System info --- */

    function loadSystemInfo() {
        fetch('/api/system/info')
            .then(function(r) { return r.json(); })
            .then(function(data) {
                var $ver = document.getElementById('sys-version');
                var $idf = document.getElementById('sys-idf');
                var $chip = document.getElementById('sys-chip');
                var $heap = document.getElementById('sys-heap');
                if ($ver) $ver.textContent = data.version || '--';
                if ($idf) $idf.textContent = data.idf_version || '--';
                if ($chip) $chip.textContent = (data.chip || 'ESP32-P4') + (data.revision ? ' ' + data.revision : '');
                if ($heap) $heap.textContent = Math.round((data.heap_free || 0) / 1024) + ' KB free';
            })
            .catch(function() { /* ignore */ });
    }

    /* ================================================================
     * Hash-based navigation
     * ================================================================ */

    function checkHash() {
        var hash = location.hash.replace('#', '');
        if (hash && ['dashboard', 'devices', 'topology', 'settings'].indexOf(hash) !== -1) {
            switchSection(hash);
        }
    }

    window.addEventListener('hashchange', checkHash);
    checkHash();

    /* ================================================================
     * Init
     * ================================================================ */

    wsConnect();
    console.log('USB/IP WebUI loaded');

})();
