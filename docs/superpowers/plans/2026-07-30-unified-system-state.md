# Unified System State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Merge `DeviceState` into `TelemetrySnapshot` so all presentation layers (MQTT, BLE, serial, OLED, HTTP, Supabase) consume a single unified state struct.

**Architecture:** `TelemetrySnapshot` gains ~15 fields from `DeviceState` (BLE/MQTT health, reset reason, crash count, NTP, SD, calibrating, min heap, OTA error). `telemetry_build()` becomes the single gather function. BLE `get_status`, serial `status`, and OLED `update_display()` all read from `TelemetrySnapshot`. `DeviceState` struct and `build_device_state()` are removed. `MQTT_LEGACY_PAYLOAD` path is removed.

**Tech Stack:** C++ structs, ArduinoJson, existing module accessors.

---

## File Map

### Modified Files
| File | Changes |
|---|---|
| `include/telemetry.h` | Add ~15 fields to `TelemetrySnapshot`, add `ota_error[64]` to `TelemetryOTA` |
| `src/telemetry.cpp` | Populate new fields in `telemetry_build()`, add `#include "ble_provisioner.h"` |
| `src/ble_provisioner.cpp` | Rewrite `get_status` handler to use `TelemetrySnapshot` instead of `DeviceState` |
| `src/main.cpp` | Rewrite `print_status()` to use `TelemetrySnapshot`, update OLED call |
| `include/display_manager.h` | Change `update_display()` signature to accept `TelemetrySnapshot` |
| `src/display_manager.cpp` | Rewrite to read from `TelemetrySnapshot` instead of ad-hoc pulls |
| `src/connectivity_manager.cpp` | Remove `MQTT_LEGACY_PAYLOAD` path |

### Deleted Files
| File | Reason |
|---|---|
| `include/device_state.h` | Merged into `TelemetrySnapshot` |
| `src/device_state.cpp` | No longer needed |

---

### Task 1: Add DeviceState fields to TelemetrySnapshot

**Files:**
- Modify: `include/telemetry.h`

Add to `TelemetryOTA`:
```cpp
    char    ota_error[64];      // last error message
```

Add to `TelemetrySnapshot` after `heap_free`:
```cpp
    uint32_t min_free_heap;
    uint8_t  reset_reason;       // 0=power-on, 1=hardware-wdt, 2=task-wdt,
                                 // 3=exception, 4=sw-reset, 5=deep-sleep
    char     hw_rev[16];
    uint32_t crash_count;
    bool     safe_mode;
    bool     ntp_synced;
    bool     ble_active;
    bool     ble_connected;
    bool     mqtt_connected;
    bool     http_configured;
    bool     supabase_configured;
    bool     network_skipped;
    bool     sd_present;
    uint8_t  log_buffer_used_pct; // 0..100
    bool     sensors_calibrating;
```

Commit:
```bash
git add include/telemetry.h
git commit -m "feat: add DeviceState fields to TelemetrySnapshot"
```

---

### Task 2: Populate new fields in telemetry_build()

**Files:**
- Modify: `src/telemetry.cpp`

Add includes:
```cpp
#include "ble_provisioner.h"
```

After the existing OTA block, add:
```cpp
    // --- System health (from DeviceState) ----------------------------------------
    out.min_free_heap = ESP.getMinFreeHeap();
    out.reset_reason = (uint8_t)esp_reset_reason();
    strncpy(out.hw_rev, get_device_hw_rev(), sizeof(out.hw_rev));
    out.hw_rev[sizeof(out.hw_rev) - 1] = '\0';
    out.crash_count = get_crash_count();
    out.safe_mode = (get_crash_count() >= 5);

    // --- WiFi / NTP ----------------------------------------------------------------
    out.ntp_synced = ntp_is_synced();

    // --- BLE -----------------------------------------------------------------------
    out.ble_active = ble_is_active();
    out.ble_connected = ble_is_connected();

    // --- Network services ----------------------------------------------------------
    out.mqtt_connected = mqtt_is_connected();
    {
        char url[128] = "";
        out.http_configured = settings_load_http_endpoint(url, nullptr, 0);
    }
    {
        char url[128] = "";
        out.supabase_configured = settings_load_supabase_url(url, sizeof(url));
    }
    out.network_skipped = network_is_skipped();

    // --- Storage --------------------------------------------------------------------
    out.sd_present = sd_is_present();
    out.log_buffer_used_pct = (uint8_t)log_buffer_used_pct();

    // --- Sensors --------------------------------------------------------------------
    out.sensors_calibrating = sensor_is_calibrating();

    // --- OTA error ------------------------------------------------------------------
    const char* ota_err = ota_get_last_error();
    if (ota_err) {
        strncpy(out.ota.ota_error, ota_err, sizeof(out.ota.ota_error));
        out.ota.ota_error[sizeof(out.ota.ota_error) - 1] = '\0';
    }
```

Commit:
```bash
git add src/telemetry.cpp
git commit -m "feat: populate health/connectivity fields in telemetry_build()"
```

---

### Task 3: Rewrite BLE get_status to use TelemetrySnapshot

**Files:**
- Modify: `src/ble_provisioner.cpp`

Find the `get_status` handler. Replace its body to build a `TelemetrySnapshot` and serialize it instead of using `DeviceState`:

```cpp
    } else if (strcmp(cmd, "get_status") == 0) {
        if (!check_pin(doc)) {
            send_error(cmd, "pin required");
            return;
        }
        TelemetrySnapshot snap;
        telemetry_build(snap);
        StaticJsonDocument<2048> resp;
        resp["ok"] = true;
        resp["cmd"] = cmd;
        resp["uptime_ms"] = snap.device.uptime_ms;
        resp["free_heap"] = snap.heap_free;
        resp["min_free_heap"] = snap.min_free_heap;
        resp["reset_reason"] = snap.reset_reason;
        resp["serial"] = snap.device.id;
        resp["hw_rev"] = snap.hw_rev;
        resp["fw"] = snap.device.fw;
        resp["crash_count"] = snap.crash_count;
        resp["safe_mode"] = snap.safe_mode;
        resp["wifi"] = snap.wifi.rssi != 0;
        resp["rssi"] = snap.wifi.rssi;
        resp["ip"] = snap.wifi.ip;
        resp["ntp"] = snap.ntp_synced;
        resp["ble"] = snap.ble_active;
        resp["ble_conn"] = snap.ble_connected;
        resp["mqtt"] = snap.mqtt_connected;
        resp["http"] = snap.http_configured;
        resp["supabase"] = snap.supabase_configured;
        resp["offline"] = snap.network_skipped;
        resp["sd"] = snap.sd_present;
        resp["entries"] = snap.log.entries;
        resp["buf_pct"] = snap.log_buffer_used_pct;
        resp["overflow"] = snap.log.overflow;
        resp["channels"] = snap.channel_count;
        resp["switches"] = snap.switch_count;
        resp["calibrating"] = snap.sensors_calibrating;
        resp["ota_state"] = snap.ota.ota_status;
        resp["ota_version"] = snap.ota.ota_version;
        resp["ota_progress"] = snap.ota.ota_progress_pct;
        resp["ota_error"] = snap.ota.ota_error;
        char buf[2048];
        size_t n = serializeJson(resp, buf, sizeof(buf));
        send_response(buf, n);
```

Also add `#include "telemetry.h"` if not already present.

Commit:
```bash
git add src/ble_provisioner.cpp
git commit -m "feat: BLE get_status now reads from TelemetrySnapshot"
```

---

### Task 4: Rewrite serial print_status to use TelemetrySnapshot

**Files:**
- Modify: `src/main.cpp`

Replace the `print_status()` function body to build a `TelemetrySnapshot` and print from it instead of using `DeviceState` + ad-hoc sensor reads:

```cpp
static void print_status() {
    TelemetrySnapshot snap;
    telemetry_build(snap);
    LOG_PRINT("── System ──────────────────────────────────────\n");
    LOG_PRINT("Uptime: %lu s  Heap: %u/%u min  Reset: %u  Crashes: %u%s\n",
        snap.device.uptime_ms / 1000, snap.heap_free, snap.min_free_heap,
        snap.reset_reason, snap.crash_count, snap.safe_mode ? " SAFE MODE" : "");
    LOG_PRINT("Device: %s rev %s fw %s\n", snap.device.id, snap.hw_rev, snap.device.fw);
    LOG_PRINT("── WiFi ────────────────────────────────────────\n");
    LOG_PRINT("Connected: %d  RSSI: %d dBm  IP: %s  NTP: %d\n",
        snap.wifi.rssi != 0, snap.wifi.rssi,
        snap.wifi.rssi != 0 ? snap.wifi.ip : "-", snap.ntp_synced);
    LOG_PRINT("── BLE ─────────────────────────────────────────\n");
    LOG_PRINT("Active: %d  Connected: %d\n", snap.ble_active, snap.ble_connected);
    LOG_PRINT("── Services ────────────────────────────────────\n");
    LOG_PRINT("MQTT: %d  HTTP: %d  Supabase: %d  Offline: %d\n",
        snap.mqtt_connected, snap.http_configured, snap.supabase_configured, snap.network_skipped);
    LOG_PRINT("── Storage ─────────────────────────────────────\n");
    LOG_PRINT("SD: %d  Log: %u entries (%u%%)  Overflow: %d\n",
        snap.sd_present, (unsigned)snap.log.entries,
        (unsigned)snap.log_buffer_used_pct, snap.log.overflow);
    LOG_PRINT("── Sensors ────────────────────────────────────\n");
    LOG_PRINT("Channels: %u  Switches: %u  Calibrating: %d\n",
        snap.channel_count, snap.switch_count, snap.sensors_calibrating);
    for (int ch = 0; ch < snap.channel_count && ch < 4; ch++) {
        const TelemetryChannel& tc = snap.channels[ch];
        LOG_PRINT("Ch%d: %.3fV %.3fA %.2fW  mAh:%.0f  Wh:%.2f\n",
            ch, tc.V, tc.I, tc.P, tc.charge_mAh, tc.energy_Wh);
    }
    LOG_PRINT("── Batteries ───────────────────────────────────\n");
    for (int i = 0; i < snap.battery_count; i++) {
        const TelemetryBattery& tb = snap.battery[i];
        LOG_PRINT("Ch%d: SoC=%.1f%%  SoH=%.1f%%  cycles=%.2f  Ah_in=%.2f  Ah_out=%.2f\n",
            tb.ch, tb.soc_pct, tb.soh_pct, tb.equivalent_full_cycles,
            tb.cumulative_Ah_in, tb.cumulative_Ah_out);
    }
    LOG_PRINT("── OTA ────────────────────────────────────────\n");
    LOG_PRINT("State: %s  Version: %s  Progress: %u%%  Error: %s\n",
        snap.ota.ota_status, snap.ota.ota_version,
        snap.ota.ota_progress_pct, snap.ota.ota_error);
}
```

Also add `#include "telemetry.h"` if not already present.

Commit:
```bash
git add src/main.cpp
git commit -m "feat: serial status now reads from TelemetrySnapshot"
```

---

### Task 5: Update OLED to use TelemetrySnapshot

**Files:**
- Modify: `include/display_manager.h`
- Modify: `src/display_manager.cpp`
- Modify: `src/main.cpp`

**display_manager.h** — change signature:
```cpp
#include "telemetry.h"
void update_display(const TelemetrySnapshot& snap);
```

**display_manager.cpp** — rewrite to read from `TelemetrySnapshot`:
```cpp
void update_display(const TelemetrySnapshot& snap) {
    // Page 0: status page
    if (g_current_page == 0) {
        draw_status_page(snap.wifi.ip, snap.channels[0].P + snap.channels[1].P + snap.channels[2].P + snap.channels[3].P,
                         temperatureRead());
        return;
    }
    // Pages 1-4: channel pages
    uint8_t ch = g_current_page - 1;
    if (ch < snap.channel_count) {
        draw_channel_page(ch, snap.channels[ch].V, snap.channels[ch].I, snap.channels[ch].P,
                          snap.channels[ch].charge_mAh);
    }
}
```

**main.cpp** — update the OLED call in networkTask:
```cpp
// Replace the old update_display call:
TelemetrySnapshot snap;
telemetry_build(snap);
update_display(snap);
```

Commit:
```bash
git add include/display_manager.h src/display_manager.cpp src/main.cpp
git commit -m "feat: OLED now reads from TelemetrySnapshot"
```

---

### Task 6: Remove DeviceState and MQTT_LEGACY_PAYLOAD

**Files:**
- Delete: `include/device_state.h`
- Delete: `src/device_state.cpp`
- Modify: `src/connectivity_manager.cpp`

**connectivity_manager.cpp** — remove the `MQTT_LEGACY_PAYLOAD` code path (the `#if MQTT_LEGACY_PAYLOAD` block in `publish_data()`).

Also remove any `#include "device_state.h"` from all files.

Commit:
```bash
git rm include/device_state.h src/device_state.cpp
git add src/connectivity_manager.cpp
git commit -m "refactor: remove DeviceState and MQTT_LEGACY_PAYLOAD"
```

---

### Task 7: Build and verify

Build all environments:
```bash
~/.platformio/venv/bin/pio run -e esp32dev
~/.platformio/venv/bin/pio run -e esp32c3
~/.platformio/venv/bin/pio run -e esp32c3_nodisplay
~/.platformio/venv/bin/pio run -e esp32s3
```

Fix any compilation errors, then commit fixes.
