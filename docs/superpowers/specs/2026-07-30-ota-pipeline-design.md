# OTA Pipeline Design

**Date:** 2026-07-30  
**Status:** Approved  
**Firmware version target:** 2.1.0  
**Backend:** Go API (self-hosted, not Supabase)

---

## 1. Overview

Add Over-The-Air firmware update capability to the ESP32 power-monitoring firmware. The backend already has OTA infrastructure (`ota_releases` table, `GET /ota/check/{key}` endpoint, MinIO binary storage). This spec covers the firmware-side OTA client and the minimal backend changes needed to support it.

---

## 2. Architecture

### New Module: `ota_client`

| File | Purpose |
|---|---|
| `include/ota_client.h` | OTA state machine, public API |
| `src/ota_client.cpp` | OTA implementation |

### State Machine

```
IDLE ──[poll timer fires]──> CHECKING ──[update avail]──> DOWNLOADING
  ▲                              │                            │
  │                         [no update]                  [streaming]
  │                              │                            │
  │                              ▼                            ▼
  │                           IDLE                     APPLYING (verify SHA256)
  │                                                         │
  │                                                   [match]│[mismatch]
  │                                                         │    │
  │                                                    REBOOTING  IDLE (abort)
  │                                                         │
  │                                                   [reboot]
  │                                                    (new fw boots)
  │                                                         │
  │                                              [mark valid] or [crash → rollback]
  └──────────────────────────────────────────────────────────┘
```

### States

| State | Description |
|---|---|
| `OTA_IDLE` | Waiting for poll timer. Timer is NVS-configurable (default 300 s). |
| `OTA_CHECKING` | HTTP GET to `GET /ota/check/{device_key}?current_ver=X.X.X`. Backend returns `{update_available, version, binary_url, sha256, binary_size, poll_interval_seconds}`. |
| `OTA_DOWNLOADING` | Streaming HTTP(S) GET from `binary_url`. Feed chunks to `esp_ota_write()` + mbedTLS SHA256 update. Report progress via MQTT status topic. |
| `OTA_APPLYING` | Download complete. Compare computed SHA256 vs expected. If match: `esp_ota_set_boot_partition()`, mark pending verify. If mismatch: abort, stay on current. |
| `OTA_REBOOTING` | `esp_restart()`. On next boot, firmware calls `esp_ota_mark_app_valid_cancel_rollback()` after successful init. If crash before that, bootloader reverts. |

### Poll Interval

- Backend includes `poll_interval_seconds` in OTA check response
- Device stores in NVS key `ota_poll_interval_s`
- Default: 300 s (5 min) if not set
- Min: 60 s, Max: 86400 s (24 h)
- Configurable at runtime via BLE command `ota_set_interval`

---

## 3. Streaming Download & SHA256 Verification

### Download Flow

```
1. WiFiClientSecure connect to binary_url (HTTPS)
2. esp_ota_begin() → get OTA handle for inactive partition
3. mbedtls_sha256_ret() init context
4. Loop: read HTTP chunk (256-1024 bytes)
   ├── esp_ota_write(handle, chunk, len)
   ├── mbedtls_sha256_update(context, chunk, len)
   └── every 10%: publish MQTT status "downloading:XX%"
5. esp_ota_end(handle)
6. mbedtls_sha256_finish() → computed_hash
7. computed_hash == expected_sha256?
   ├── YES: esp_ota_set_boot_partition(), mark pending verify, reboot
   └── NO:  log error, stay on current firmware
```

### Memory

- No large buffer — streams through a small chunk buffer (~1 KB stack)
- SHA256 context: ~100 bytes heap
- OTA handle: pointer

### Timeout Strategy

- HTTP connect timeout: 10 s
- Read timeout: 5 s per chunk
- Total download timeout: 5 min for 1.9 MB @ ~50 KB/s = ~40 s typical
- If any timeout: abort OTA, log event, stay on current firmware

### Error Handling

| Failure | Action |
|---|---|
| HTTP connect timeout | Abort, log event, return to IDLE |
| HTTP non-200 response | Abort, log event, return to IDLE |
| esp_ota_begin() fail | Abort, log event, return to IDLE |
| esp_ota_write() fail | Abort, log event, return to IDLE |
| SHA256 mismatch | Abort, log event, return to IDLE |
| Download interrupted mid-way | Abort, log event, return to IDLE. Next poll will retry. |

---

## 4. ESP32 Native Rollback

### Boot Flow After OTA

```
ESP32 boots
  ├── bootloader checks OTA info
  │   ├── new firmware marked "pending verify" → boot into new firmware
  │   └── previous firmware → boot into old firmware
  │
  ├── setup() runs
  │   ├── init_settings()
  │   ├── init_event_log()
  │   ├── init_device_identity()
  │   ├── init_sensors()
  │   ├── init_connectivity()
  │   ├── NTP sync succeeds
  │   └── → esp_ota_mark_app_valid_cancel_rollback()
  │       └── firmware is now "confirmed" — bootloader will not revert
  │
  └── If crash before mark_valid:
      ├── crash counter increments
      ├── bootloader reverts to previous OTA slot
      └── old firmware boots with crash_count > 0
```

### Rollback Detection

- On boot after OTA (detected by checking `esp_ota_get_state_partition()` returns `ESP_OTA_IMG_PENDING_VERIFY`):
  - If init succeeds through NTP sync → `esp_ota_mark_app_valid_cancel_rollback()`
  - If crash before that → bootloader auto-reverts
- If crash count ≥ 5 before mark_valid → safe mode on old firmware

### Integration with Existing Crash Counter

- `mark_clean_shutdown()` is called before OTA reboot
- On successful boot after OTA, crash counter is reset
- The existing safe mode (5+ crashes) still works — if the new firmware keeps crashing before mark_valid, the bootloader reverts AND the crash counter eventually triggers safe mode on the old firmware

---

## 5. Integration Points

### Network Task

```cpp
// In networkTask loop (every 10 ms):
loop_connectivity();
loop_ble_provisioner();
loop_ota_client();       // ← NEW: drives OTA state machine
// ...existing display/queue/poll code...
```

### Safe Mode

- OTA client is NOT started in safe mode (5+ crashes)
- This prevents an OTA-triggered crash loop from re-applying the same bad update

### Event Log

```cpp
log_event(INFO,  "ota", "checking for updates");
log_event(INFO,  "ota", "downloading v%s (%d%%)", version, pct);
log_event(INFO,  "ota", "update to v%s applied, rebooting", version);
log_event(ERROR, "ota", "SHA256 mismatch, aborting");
log_event(ERROR, "ota", "download failed: %s", reason);
```

### Telemetry Snapshot

Add to `TelemetrySnapshot`:

```cpp
struct TelemetryOTA {
    bool    ota_in_progress;    // true while downloading/applying
    char    ota_version[16];    // version being applied
    uint8_t ota_progress_pct;   // 0..100 during download
    char    ota_status[12];     // "idle", "checking", "downloading",
                                // "applying", "rebooting", "failed"
};
```

### MQTT Status Topic

Publish to `status/{device_key}/ota`:

```json
{"status":"checking","current_ver":"2.0.0"}
{"status":"downloading","version":"2.1.0","progress":45}
{"status":"applied","version":"2.1.0","sha256":"abc123..."}
{"status":"failed","version":"2.1.0","error":"SHA256 mismatch"}
```

### BLE Commands

| Command | Description |
|---|---|
| `ota_check` | Trigger immediate OTA poll (bypass timer) |
| `ota_status` | Return current OTA state and version info |
| `ota_set_interval N` | Set poll interval in seconds (60-86400) |

---

## 6. Backend Changes

### OTA Check Response

Add `poll_interval_seconds` to `GET /ota/check/{key}` response:

```json
{
  "update_available": true,
  "version": "2.1.0",
  "binary_url": "https://firmware.example.com/bucket/fw-v2.1.0.bin",
  "sha256": "abc123...",
  "binary_size": 1900000,
  "poll_interval_seconds": 300
}
```

When `update_available: false`, still include `poll_interval_seconds` so the device can adjust its polling cadence even without an update.

### MQTT Topic

New topic: `status/{device_key}/ota` — backend should consume this to track rollout progress per device. The existing `device_commands` table can be used to trigger immediate OTA checks.

---

## 7. Files to Create/Modify

### New Files

| File | Lines (est.) | Purpose |
|---|---|---|
| `include/ota_client.h` | ~80 | OTA state machine enum, public API declarations |
| `src/ota_client.cpp` | ~400 | OTA implementation: state machine, HTTP download, SHA256, esp_ota |

### Modified Files

| File | Changes |
|---|---|
| `include/telemetry.h` | Add `TelemetryOTA` struct, add `ota` field to `TelemetrySnapshot` |
| `src/telemetry.cpp` | Populate OTA fields in `telemetry_build()` |
| `include/connectivity_manager.h` | Add `publish_ota_status()` declaration |
| `src/connectivity_manager.cpp` | Implement `publish_ota_status()` MQTT publish |
| `include/ble_provisioner.h` | Add `ota_check`, `ota_status`, `ota_set_interval` to command table |
| `src/ble_provisioner.cpp` | Implement OTA BLE command handlers |
| `src/main.cpp` | Add `loop_ota_client()` to network task, rollback confirmation in `setup()` |
| `include/config.h` | Add OTA defaults: `OTA_POLL_INTERVAL_S`, `OTA_HTTP_TIMEOUT_MS`, `OTA_CHUNK_SIZE` |
| `include/settings_manager.h` | Add `settings_load_ota_poll_interval()`, `settings_save_ota_poll_interval()` |
| `src/settings_manager.cpp` | Implement OTA NVS persistence |
| `platformio.ini` | Switch all envs to OTA partition layouts (`partitions_ota_4m.csv` / `partitions_ota_8m.csv`) |
| `backend/internal/ota.go` | Add `poll_interval_seconds` to `OTACheckResponse` and `CheckOTA` response |

---

## 8. Build Configuration

### Partition Layouts

All environments switch to OTA-capable partition tables:

| Env | Current | New | OTA Slot Size |
|---|---|---|---|
| `esp32dev` | `min_spiffs.csv` | `partitions_ota_4m.csv` | 2 × 1.9 MB |
| `esp32c3` | `min_spiffs.csv` | `partitions_ota_4m.csv` | 2 × 1.9 MB |
| `esp32c3_nodisplay` | `min_spiffs.csv` | `partitions_ota_4m.csv` | 2 × 1.9 MB |
| `esp32s3` | `partitions_ota_8m.csv` | `partitions_ota_8m.csv` | 2 × 3.7 MB (no change) |

The `min_spiffs.csv` partition file is no longer used and can be removed.

### Firmware Version

- `TELEMETRY_FW_VERSION` is the canonical version string sent to the backend
- The OTA check passes `current_ver=TELEMETRY_FW_VERSION` as a query parameter
- Production builds should set a unique version via `build_flags`:
  ```
  -D TELEMETRY_FW_VERSION="\"2.1.0\""
  ```

---

## 9. Rollout Sequence

1. **Build firmware v2.1.0** with OTA client code
2. **Flash v2.1.0 via serial** to all devices (initial OTA-capable build must be serial-flashed)
3. **Create OTA release v2.1.1** in backend (upload binary, set channel=stable)
4. **Devices poll** → detect v2.1.1 → download → apply → reboot → confirm
5. **Monitor** OTA status via MQTT `status/{key}/ota` topic
6. **If issues:** fix in v2.1.2, create release, devices auto-update

---

## 10. Testing Strategy

### Unit Tests (sim harness)
- OTA state machine transitions (IDLE → CHECKING → DOWNLOADING → APPLYING → REBOOTING)
- SHA256 computation against known test vectors
- Poll interval clamping (min/max bounds)
- Semver comparison (reuse backend logic)

### Hardware Tests
- OTA from v2.0.0 → v2.1.0 over WiFi (real MinIO URL)
- OTA with SHA256 mismatch (corrupted binary) → verify abort + stay on current
- OTA with network interruption mid-download → verify retry on next poll
- Rollback test: flash bad firmware via OTA, confirm bootloader reverts
- Rollback test: flash good firmware, confirm `mark_app_valid_cancel_rollback()` works
- Safe mode: 5+ crashes after OTA → safe mode on old firmware
