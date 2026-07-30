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
| `OTA_DOWNLOADING` | Non-blocking chunked HTTP(S) GET from `binary_url`. One chunk per `loop_ota_client()` call. Feed chunks to `esp_ota_write()` + mbedTLS SHA256 update. Report progress via MQTT status topic. HTTP client + OTA handle + SHA256 context persist across calls. |
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

### Download Flow (Non-Blocking)

The download is NOT a blocking loop. It runs one chunk per `loop_ota_client()` call so the network task stays responsive (MQTT, BLE, display, Supabase all continue). State is preserved across calls via static variables:

```
OTA_DOWNLOADING state, per tick:
  ├── [first call] WiFiClientSecure connect to binary_url (HTTPS)
  ├── [first call] esp_ota_begin() → OTA handle for inactive partition
  ├── [first call] mbedtls_sha256_ret() init context
  ├── [first call] total_bytes_written = 0
  │
  ├── [every call] esp_task_wdt_reset()  ← prevents 30s TWDT trip
  ├── [every call] read one HTTP chunk (256-1024 bytes)
  │   ├── chunk received → esp_ota_write(handle, chunk, len)
  │   │                    mbedtls_sha256_update(context, chunk, len)
  │   │                    total_bytes_written += len
  │   │                    every 10%: publish MQTT status "downloading:XX%"
  │   └── no chunk yet (TCP buffer empty) → return, try again next tick
  │
  └── [last call] HTTP response fully consumed
      ├── esp_ota_end(handle)
      ├── mbedtls_sha256_finish() → computed_hash
      ├── total_bytes_written == binary_size?
      │   ├── NO → log error, abort, return to IDLE
      │   └── YES → continue
      ├── computed_hash == expected_sha256?
      │   ├── YES: esp_ota_set_boot_partition(), mark pending verify, reboot
      │   └── NO:  log error, abort, return to IDLE
      └── cleanup: close HTTP, free OTA handle, zero static state
```

### Memory

- No large buffer — streams through a small chunk buffer (~1 KB stack)
- SHA256 context: ~100 bytes heap
- OTA handle: pointer
- HTTP client + response stream: ~500 bytes heap (reuses existing `WiFiClientSecure`)

### Timeout Strategy

- HTTP connect timeout: 10 s
- Read timeout: 5 s per chunk (per `loop_ota_client()` tick)
- Total download timeout: 5 min for 1.9 MB @ ~50 KB/s = ~40 s typical
- If any timeout: abort OTA, log event, stay on current firmware
- Timeout is tracked as elapsed wall-clock since download started; if exceeded, abort

### Error Handling

| Failure | Action |
|---|---|
| HTTP connect timeout | Abort, log event, return to IDLE |
| HTTP non-200 response | Abort, log event, return to IDLE |
| esp_ota_begin() fail | Abort, log event, return to IDLE |
| esp_ota_write() fail | Abort, log event, return to IDLE |
| SHA256 mismatch | Abort, log event, return to IDLE |
| Size mismatch (downloaded != binary_size) | Abort, log event, return to IDLE |
| Download interrupted mid-way | Abort, log event, return to IDLE. Next poll will retry. |
| Total download timeout exceeded | Abort, log event, return to IDLE |

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
  │   ├── init_switches()
  │   ├── init_core_shared()
  │   ├── sensor task starts, first read_sensors() succeeds
  │   ├── first evaluate_switches() pass completes
  │   ├── 60-second grace timer expires (fed by esp_task_wdt_reset)
  │   └── → esp_ota_mark_app_valid_cancel_rollback()
  │       └── firmware is now "confirmed" — bootloader will not revert
  │
  └── If crash before mark_valid:
      ├── crash counter increments
      ├── bootloader reverts to previous OTA slot
      └── old firmware boots with crash_count > 0
```

**Why firmware-health signal instead of NTP sync:** Network reachability (NTP, WiFi) is an environmental condition, not a firmware-health condition. A healthy firmware that boots on a network-less site should not roll back. The mark_valid trigger is: sensors init + first sensor read + first switch eval + 60s grace timer. This proves the firmware core loop works. Network services are started in parallel but do not gate the rollback confirmation.

### Rollback Detection

- On boot after OTA (detected by checking `esp_ota_get_state_partition()` returns `ESP_OTA_IMG_PENDING_VERIFY`):
  - If firmware-health signal fires (sensors init + first read + first switch eval + 60s grace) → `esp_ota_mark_app_valid_cancel_rollback()`
  - If crash before that → bootloader auto-reverts
- If crash count ≥ 5 before mark_valid → safe mode on old firmware

### Integration with Existing Crash Counter

- `mark_clean_shutdown()` is called before OTA reboot
- On successful boot after OTA, crash counter is reset
- The existing safe mode (5+ crashes) still works — if the new firmware keeps crashing before mark_valid, the bootloader reverts AND the crash counter eventually triggers safe mode on the old firmware

### Safe Mode Tradeoff

A bad OTA that bootloops 5 times will:
1. Increment crash_count to 5 on the new firmware
2. Bootloader reverts to old firmware
3. Old firmware sees crash_count ≥ 5 → enters safe mode (no network, no BLE)
4. Recovery requires serial reflash or factory reset

This is an acceptable last-resort safety: a persistently crashing firmware cannot keep re-applying itself via OTA. The tradeoff is that a healthy old firmware is temporarily locked in safe mode. The user must serial-flash a known-good build to recover. This is consistent with the existing safe mode behavior.

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
    char    ota_status[16];     // "idle", "checking", "downloading",
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

### Reentrancy / Idempotency

- While state is CHECKING, DOWNLOADING, APPLYING, or REBOOTING: ignore all new triggers (poll timer, BLE `ota_check`, Supabase command)
- Only IDLE state accepts triggers
- State transitions are atomic (single-task, no concurrency concern — all OTA runs on the network task)

### TLS Posture

The firmware uses `WiFiClientSecure::setInsecure()` for all HTTPS connections (telemetry, Supabase, and OTA check/download). This accepts any server certificate — no CA pinning. The security model for OTA relies on:

1. **SHA256 verification** of the downloaded binary against the hash returned by the backend's `/ota/check` endpoint
2. **Integrity chain**: if an attacker MITMs the `/ota/check` response, they can substitute both the `binary_url` and the `sha256`, defeating verification. This is a pre-existing limitation shared with all other HTTPS paths in the firmware.

For production deployments requiring stronger guarantees, replace `setInsecure()` with `setCACert()` using the MinIO/backend CA certificate. This is a config change, not an architecture change.

### BLE Commands

| Command | Description |
|---|---|
| `ota_check` | Trigger immediate OTA poll (bypass timer) |
| `ota_status` | Return current OTA state and version info |
| `ota_set_interval N` | Set poll interval in seconds (60-86400) |

---

## 6. Backend Changes

### OTA Check Response

Add `poll_interval_seconds` to `GET /ota/check/{key}` response. The value comes from the `ota_releases` table's `rollout_pct`-related config or a global/org-level default. If not set, the device uses its NVS default (300 s).

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

### Downgrade / Rollback Releases

The `ota_releases` table has an `is_rollback` column. The backend's `semverGreater` check only offers updates when `release > current`, so rollback releases (lower version) are never offered to devices. If forced downgrade is needed:

- Backend: add a `force` flag to the check response that bypasses semver comparison
- Firmware: accept the binary regardless of version when `force: true`

This is not implemented in the initial version. The initial version only upgrades (semver greater).

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
| `backend/internal/ota.go` | Add `poll_interval_seconds` to `OTACheckResponse` and `CheckOTA` response |

---

## 8. Build Configuration

### Partition Layouts

All environments already use OTA-capable partition tables (no change needed):

| Env | Partition Table | OTA Slot Size |
|---|---|---|
| `esp32dev` | `partitions_ota_4m.csv` | 2 × 1.9 MB |
| `esp32c3` | `partitions_ota_4m.csv` | 2 × 1.9 MB |
| `esp32c3_nodisplay` | `partitions_ota_4m.csv` | 2 × 1.9 MB |
| `esp32s3` | `partitions_ota_8m.csv` | 2 × 3.7 MB |

The stale `min_spiffs.csv` file was already deleted from the working tree. The `CLAUDE.md` reference to `min_spiffs.csv` should be updated to `partitions_ota_4m.csv`.

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
