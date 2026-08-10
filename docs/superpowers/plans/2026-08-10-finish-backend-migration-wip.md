# Finish Backend-Migration WIP + PCF8574AT Fault-Tolerance Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the in-flight Supabase→backend telemetry migration correctly (port the cloud command-poll instead of dropping it, remove genuinely dead code, preserve two Supabase side-effect syncs that have no backend replacement yet) and fix a fault-tolerance bug in the new PCF8574AT I2C expander driver.

**Architecture:** No new modules. All changes are edits to `src/connectivity_manager.cpp` / `include/connectivity_manager.h` (dead code removal + new backend HTTP command-poll functions), `src/main.cpp` (re-wire the poll call, fix a comment), `src/pcf8574at.cpp` / `include/pcf8574at.h` (shadow-state fix), and `src/switch_controller.cpp` (check the driver's new return values).

**Tech Stack:** ESP32 Arduino framework (C++17), ArduinoJson v7, HTTPClient/WiFiClientSecure, PlatformIO, host-side `sim/` test harness (g++, no hardware).

**Spec:** `docs/superpowers/specs/2026-08-10-finish-backend-migration-wip-design.md`

---

## Correction vs. the approved spec

While gathering exact code for this plan, a second finding surfaced that the approved spec's Task-1 description ("`publish_data_supabase()` is fully redundant, safe to delete") got wrong: that function has two side effects with no other call site anywhere in the codebase — `publish_battery_profiles_heartbeat()` (60s heartbeat + eager-on-change sync of battery profiles to Supabase) and `publish_calibration_status()` (live calibration-progress sync to Supabase, used by the dashboard while a calibration run is active). The new Go backend has no equivalent endpoints for either. Deleting `publish_data_supabase()` outright would silently kill both, the same class of regression the spec already flagged for `check_settings_commands()`.

Task 1 below deletes `publish_data_supabase()` (it genuinely duplicates telemetry, which is the only truly-redundant part) but relocates the two side-effect calls into `publish_data()`, gated on Supabase still being configured, so no capability is lost. This is strictly more conservative than what was approved, not a scope expansion — flagging it here for the record rather than re-opening brainstorming.

---

### Task 1: Remove dead Supabase code, preserve battery-profile/calibration side effects

**Files:**
- Modify: `src/connectivity_manager.cpp`
- Modify: `include/connectivity_manager.h`

- [ ] **Step 1: Delete the dead log-batch cluster**

In `src/connectivity_manager.cpp`, find this block (currently lines ~1045–1329, right after `telemetry_kick_battery_profiles()` and right before `static JsonDocument g_pub_doc;`):

```cpp
static JsonDocument g_supa_doc;

#define LOG_BATCH_SIZE 10
// send_log_entry / flush_log_batch each construct their own local
// StaticJsonDocument. The previous static g_log_doc was shared state that
// every caller had to remember to .clear() — easy to miss, and a corruption
...
[everything through the end of publish_log_batch_supabase(), i.e. flush_log_batch(),
 send_log_entry(), decode_and_send_log_entries(), publish_log_batch_supabase(),
 and the g_log_count / g_log_last_ts / g_log_ts_untrusted_count declarations]
...
void publish_log_batch_supabase() {
    ...
    if (state == ST_DONE) {
        ...
    }
}
```

Delete the entire block, from `static JsonDocument g_supa_doc;` through the closing `}` of `publish_log_batch_supabase()`. Leave `static JsonDocument g_pub_doc;` (the next line) and everything after it untouched — `g_pub_doc` belongs to `drain_pending_telemetry_overflow()`, which stays.

Use this to locate it precisely:

```bash
grep -n "static JsonDocument g_supa_doc;\|^static JsonDocument g_pub_doc;" src/connectivity_manager.cpp
```

Delete everything from the first match's line to one line before the second match's line.

- [ ] **Step 2: Delete `publish_data_supabase()`, relocating its two side effects**

Find (currently lines ~1459–1554):

```cpp
void publish_data_supabase(const SensorSnapshot& data) {
    ...
}

void publish_data_supabase(const SensorSnapshot& data, const TelemetrySnapshot& snap) {
    ...
    // Slow path: battery profile heartbeat (60s) and eager on change.
    publish_battery_profiles_heartbeat(supabase_url, anon_key);

    publish_calibration_status();
}
```

Delete both function definitions entirely (from `void publish_data_supabase(const SensorSnapshot& data) {` through the closing `}` of the second overload, right before the `// g_rpc_doc — shared RPC payload buffer...` comment, which stays).

- [ ] **Step 3: Re-add the two side effects at the end of `publish_data()`**

In the same file, find the end of `publish_data(const SensorSnapshot& data, const TelemetrySnapshot& snap)`:

```cpp
    static char ble_buf[256];
    size_t blen = serialize_telemetry_ble(snap, ble_buf, sizeof(ble_buf));
    if (blen > 0) {
        ble_notify_sensor_data(ble_buf, blen);
    } else {
        LOG_PRINTLN("[BLE] telemetry subset overflow — notify dropped (MTU too small for full payload)");
    }
    vTaskDelay(pdMS_TO_TICKS(25));  // space out notifies — avoids BLE stack crowding / UX jitter
}
```

Replace the closing `}` with:

```cpp
    static char ble_buf[256];
    size_t blen = serialize_telemetry_ble(snap, ble_buf, sizeof(ble_buf));
    if (blen > 0) {
        ble_notify_sensor_data(ble_buf, blen);
    } else {
        LOG_PRINTLN("[BLE] telemetry subset overflow — notify dropped (MTU too small for full payload)");
    }
    vTaskDelay(pdMS_TO_TICKS(25));  // space out notifies — avoids BLE stack crowding / UX jitter

    // Legacy Supabase side syncs with no backend-native replacement yet:
    // battery-profile heartbeat and live calibration-progress. Both used to
    // run inside the now-removed publish_data_supabase(); keep them firing
    // for any device that still has Supabase configured so the dashboard
    // doesn't lose these two views. publish_calibration_status() loads its
    // own credentials and no-ops when nothing is configured or no
    // calibration is active.
    char supabase_url[128], supabase_anon_key[128];
    if (settings_load_supabase_url(supabase_url, sizeof(supabase_url)) &&
        settings_load_supabase_anon_key(supabase_anon_key, sizeof(supabase_anon_key))) {
        publish_battery_profiles_heartbeat(supabase_url, supabase_anon_key);
    }
    publish_calibration_status();
}
```

- [ ] **Step 4: Update `connectivity_manager.h`**

Delete these two lines:

```cpp
void publish_data_supabase(const SensorSnapshot& data);
void publish_data_supabase(const SensorSnapshot& data, const TelemetrySnapshot& snap);
```

Delete this line:

```cpp
void publish_log_batch_supabase();
```

Leave `void publish_log_batch();` (the line above it) — that's the separate, pre-existing, out-of-scope MQTT log-batch function noted in the spec.

- [ ] **Step 5: Rebuild and verify**

```bash
cd sim && make clean >/dev/null && make all && make test_publish_path && make test_cycle_counter && make test_data_logger && make test_bl0939_crc
```

Expected: all four suites report `N/N tests passed, 0 failed` (currently 103, 48, 8, 4 — counts must not drop, since no test referenced the deleted functions).

```bash
cd /home/sayem/sources/power-monitoring && ~/.platformio/venv/bin/pio run -e esp32dev -e esp32c3 -e esp32c3_nodisplay
```

Expected: `3 succeeded` — no undefined-reference or unused-declaration errors.

- [ ] **Step 6: Commit**

```bash
git add src/connectivity_manager.cpp include/connectivity_manager.h
git commit -m "$(cat <<'EOF'
refactor(firmware): remove dead Supabase telemetry/log-batch publish paths

publish_data_supabase() and publish_log_batch_supabase() (plus their
private helpers flush_log_batch/send_log_entry/decode_and_send_log_entries)
had no callers left after the MQTT telemetry migration — SD-card daily CSV
logging is now the durable local store, so the log-batch cloud backfill
transport isn't needed. Battery-profile-heartbeat and calibration-status
syncs (side effects of the deleted function, no backend-native replacement
yet) are preserved by moving them into publish_data().
EOF
)"
```

---

### Task 2: Add backend command-queue HTTP helpers

**Files:**
- Modify: `src/connectivity_manager.cpp`

- [ ] **Step 1: Add `backend_http_prepare`, `backend_get`, `backend_post`**

Find the end of `supabase_get()`:

```cpp
static int supabase_get(const char* url_path, const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!supabase_http_prepare(full_url, anon_key)) return -1;
    int rc = g_supa_http.GET();
    if (rc < 0) supabase_http_reset();
    return rc;
}
```

Insert immediately after it:

```cpp
// ── New backend (Go API) command-queue HTTP helpers ────────────────────────
// The new backend authenticates firmware requests with X-Device-Key /
// X-Api-Key headers (see backend/internal/middleware.go DeviceAuthMiddleware),
// not the Supabase apikey/Authorization pair. Reuses the g_supa_http /
// g_supa_client connection-teardown machinery above (already the right
// pattern for low-frequency polls) with a different header set.
static bool backend_http_prepare(const char* full_url, const char* device_key, const char* api_key) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_LOWFREQ) return false;
    if (g_supa_http_ready) {
        supabase_http_reset();
    }
    static bool g_supa_client_configured = false;
    if (!g_supa_client_configured) {
        g_supa_client.setInsecure();
        g_supa_client.setHandshakeTimeout(10);
        g_supa_client_configured = true;
    }
    g_supa_http.setReuse(false);
    g_supa_http.setTimeout(4000);
    if (!g_supa_http.begin(g_supa_client, full_url)) {
        LOG_PRINT("[BACKEND_HTTP] begin failed: %s\n", full_url);
        supabase_http_reset();
        return false;
    }
    g_supa_http.addHeader("Content-Type", "application/json");
    g_supa_http.addHeader("X-Device-Key", device_key);
    g_supa_http.addHeader("X-Api-Key", api_key);
    g_supa_http_ready = true;
    return true;
}

static int backend_get(const char* url_path, const char* backend_url,
                        const char* device_key, const char* api_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", backend_url, url_path);
    if (!backend_http_prepare(full_url, device_key, api_key)) return -1;
    int rc = g_supa_http.GET();
    if (rc < 0) supabase_http_reset();
    return rc;
}

static int backend_post(const char* url_path, const char* payload, size_t len,
                         const char* backend_url, const char* device_key, const char* api_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", backend_url, url_path);
    if (!backend_http_prepare(full_url, device_key, api_key)) return -1;
    int rc = g_supa_http.POST((uint8_t*)payload, len);
    if (rc < 0) {
        supabase_http_reset();
        delay(50);
        if (!backend_http_prepare(full_url, device_key, api_key)) return -1;
        rc = g_supa_http.POST((uint8_t*)payload, len);
    }
    drain_response();
    supabase_http_reset();
    return rc;
}
```

- [ ] **Step 2: Rebuild (compile-only check — nothing calls these yet)**

```bash
cd sim && make clean >/dev/null && make all
```

Expected: builds clean. `backend_http_prepare`/`backend_get`/`backend_post` are `static` with no caller yet — g++ will not warn about unused static functions that are merely unreferenced-but-could-be-used-later only if truly unused; if you see `-Wunused-function`, that's expected until Task 3 adds the caller. Do not silence it — Task 3 removes the warning by using them.

- [ ] **Step 3: Commit**

```bash
git add src/connectivity_manager.cpp
git commit -m "feat(firmware): add backend command-queue HTTP helpers (X-Device-Key auth)"
```

---

### Task 3: Port `check_settings_commands()` to the new backend

**Files:**
- Modify: `src/connectivity_manager.cpp`

- [ ] **Step 1: Replace the function body**

Find the current `check_settings_commands()` (locate with `grep -n "^void check_settings_commands" src/connectivity_manager.cpp`) and delete its entire body, from `void check_settings_commands() {` through its closing `}` (the function that POSTs to `/rest/v1/rpc/claim_settings_command` and ends with the `settings_fail_count` block).

Replace it with:

```cpp
void check_settings_commands() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PUBLISH) return;

    static unsigned long last_check = 0;
    if (millis() - last_check < 5000) return;  // poll every 5s
    last_check = millis();

    char backend_url[128], device_key[64], api_key[64];
    if (!settings_load_ota_backend_url(backend_url, sizeof(backend_url))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    char path[128];
    snprintf(path, sizeof(path), "/commands/%s/pending", device_key);
    int rc = backend_get(path, backend_url, device_key, api_key);
    if (rc != 200) {
        if (rc > 0) {
            LOG_PRINT("[CMD] pending-poll failed: HTTP %d\n", rc);
        }
        supabase_http_reset();
        return;
    }

    // Heap-allocate the response body — keeps this function's stack frame
    // small on the 4 KB network-task stack (same reasoning as the old
    // Supabase claim path this replaces).
    const size_t BODY_CAP = 2048;
    char* body = (char*)malloc(BODY_CAP);
    if (!body) { supabase_http_reset(); return; }
    size_t body_len = 0;
    Stream& stream = g_supa_http.getStream();
    unsigned long t0 = millis();
    while (stream.available() && body_len < BODY_CAP - 1 && millis() - t0 < 2000) {
        int c = stream.read();
        if (c >= 0) body[body_len++] = (char)c;
    }
    body[body_len] = '\0';
    supabase_http_reset();

    if (body_len == 0) { free(body); return; }

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, body);
    free(body);
    if (err) {
        LOG_PRINT("[CMD] pending-poll JSON parse error: %s\n", err.c_str());
        return;
    }

    JsonArray commands = doc.as<JsonArray>();
    if (commands.isNull()) return;

    // Bound the work done per poll tick — a burst of queued commands should
    // drain over a few ticks rather than blocking the network task for one
    // long tick (mirrors the 5-entries-per-call pattern used elsewhere in
    // this file for log draining).
    uint8_t processed = 0;
    for (JsonObject cmd : commands) {
        if (++processed > 4) break;

        long long cmd_id = cmd["id"] | 0LL;
        const char* cmd_type = cmd["cmd_type"] | "";
        if (cmd_id == 0 || strlen(cmd_type) == 0) continue;

        const size_t PAY_CAP = 1024;
        char* payload_buf = (char*)malloc(PAY_CAP);
        if (!payload_buf) continue;
        JsonVariant payload_var = cmd["payload"];
        if (payload_var.is<const char*>()) {
            strlcpy(payload_buf, payload_var.as<const char*>(), PAY_CAP);
        } else {
            serializeJson(payload_var, payload_buf, PAY_CAP);
        }

        bool applied = apply_settings_command(cmd_type, payload_buf);
        if (applied) {
            apply_settings_posthook(cmd_type);
            g_deferred_requests |= 1;  // sync_device_channels

            // Immediate relay energize/de-energize: set_relay carries
            // is_energized directly (distinct from the rule-config fields
            // apply_settings_command already persisted above).
            if (strcmp(cmd_type, "set_relay") == 0) {
                if (JsonObject obj = payload_var.as<JsonObject>()) {
                    uint8_t idx = obj["idx"] | 0;
                    if (!obj["is_energized"].isNull()) {
                        bool energize = obj["is_energized"].as<bool>();
                        switch_set(idx, energize);  // toggles GPIO + publishes state
                        g_deferred_requests &= ~4;  // switch_set already published
                    } else {
                        g_deferred_relay_idx = idx;
                        g_deferred_relay_state = get_switch_state(idx);
                        g_deferred_requests |= 4;  // sync switch state via deferred path
                    }
                }
            }
        } else {
            LOG_PRINT("[CMD] apply failed for %s\n", cmd_type);
        }
        free(payload_buf);

        // Report the outcome back to the backend so the web UI's command
        // history reflects reality (the old Supabase path never reported
        // results — this is new, backend-required behavior).
        StaticJsonDocument<256> result_doc;
        result_doc["status"] = applied ? "applied" : "failed";
        if (!applied) result_doc["error"] = "apply_settings_command failed";
        char result_buf[256];
        size_t result_len = serializeJson(result_doc, result_buf);
        char result_path[64];
        snprintf(result_path, sizeof(result_path), "/commands/%lld/result", cmd_id);
        int result_rc = backend_post(result_path, result_buf, result_len, backend_url, device_key, api_key);
        if (result_rc < 200 || result_rc >= 300) {
            LOG_PRINT("[CMD] result report failed for id=%lld: HTTP %d\n", cmd_id, result_rc);
        }
    }
}
```

- [ ] **Step 2: Rebuild**

```bash
cd sim && make clean >/dev/null && make all && make test_publish_path
```

Expected: builds clean, `103/103 tests passed, 0 failed` (no sim test exercises this function directly — the sim harness stubs HTTP entirely — so this is a compile-correctness check, not new behavioral coverage; that gap is called out in the spec as accepted, backend-integration-testing is out of scope for this pass).

```bash
cd /home/sayem/sources/power-monitoring && ~/.platformio/venv/bin/pio run -e esp32dev -e esp32c3 -e esp32c3_nodisplay
```

Expected: `3 succeeded`.

- [ ] **Step 3: Commit**

```bash
git add src/connectivity_manager.cpp
git commit -m "$(cat <<'EOF'
feat(firmware): port check_settings_commands() to the new backend

Was left disconnected by the telemetry migration, which meant the cloud
had no way to push config to a device. Polls GET /commands/{key}/pending
and reports outcomes via POST /commands/{id}/result instead of the retired
Supabase claim_settings_command RPC. Preserves the existing
apply_settings_command/apply_settings_posthook path and the immediate
relay-energize special case unchanged.
EOF
)"
```

---

### Task 4: Re-wire the call in `main.cpp` and fix the stale comment

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Fix the `networkTask` header comment**

Find:

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Core 0 — Network Task
// Handles: MQTT loop, HTTP publish, Supabase telemetry + settings poll + OLED display
// ─────────────────────────────────────────────────────────────────────────────
```

Replace with:

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Core 0 — Network Task
// Handles: MQTT telemetry publish, backend command poll, OLED display
// ─────────────────────────────────────────────────────────────────────────────
```

- [ ] **Step 2: Re-add the command-poll call**

Find:

```cpp
            TelemetrySnapshot snap;
            telemetry_build(snap);
            publish_data(data, snap);

            // Update OLED from network task so I2C display traffic doesn't delay
            // the 1-second sensor sampling loop.
            if (millis() - last_display_update >= 5000) {
                last_display_update = millis();
                update_display(snap);
            }
        }

        // Retry NTP sync every 60s if not yet synced (SNTP runs in background)
```

Replace with:

```cpp
            TelemetrySnapshot snap;
            telemetry_build(snap);
            publish_data(data, snap);

            // Update OLED from network task so I2C display traffic doesn't delay
            // the 1-second sensor sampling loop.
            if (millis() - last_display_update >= 5000) {
                last_display_update = millis();
                update_display(snap);
            }
        }

        // Poll the backend for pending settings commands (rate-limited to
        // 5s internally).
        check_settings_commands();

        // Retry NTP sync every 60s if not yet synced (SNTP runs in background)
```

- [ ] **Step 3: Rebuild**

```bash
cd sim && make clean >/dev/null && make all
cd /home/sayem/sources/power-monitoring && ~/.platformio/venv/bin/pio run -e esp32dev -e esp32c3 -e esp32c3_nodisplay
```

Expected: sim builds clean; `3 succeeded` for PlatformIO.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "fix(firmware): re-wire check_settings_commands() into networkTask, fix stale comment"
```

---

### Task 5: Fix the PCF8574AT shadow-state bug

**Files:**
- Modify: `include/pcf8574at.h`
- Modify: `src/pcf8574at.cpp`

- [ ] **Step 1: Update the header**

Replace the full contents of `include/pcf8574at.h` with:

```cpp
#ifndef PCF8574AT_H
#define PCF8574AT_H

#include <stdint.h>
#include <stdbool.h>

// ── PCF8574AT I2C I/O Expander Driver ──────────────────────────────────────
//
// The PCF8574AT is an 8-bit I2C I/O expander. It drives 8 outputs (or reads
// 8 inputs) over a single I2C address. This driver implements output mode for
// relay control.
//
// I2C address: 0x38-0x3F depending on A0-A2 pin strapping.
//   Default (all high = VCC): 0x3F
//   All low (GND):             0x38
//
// Write: send one byte — each bit corresponds to an I/O pin (bit 0 = P0).
// Read:  read one byte — returns the pin states.
//
// The PCF8574AT has a quasi-bidirectional architecture: when a pin is set HIGH
// it can source only ~100 µA, so outputs driving relays should be active-LOW
// (relay ON = pin LOW) with external pull-up resistors, OR the expander output
// should drive a transistor/ MOSFET that switches the relay coil.
//
// This driver assumes the shared `Wire` bus is already initialized by
// sensor_manager.cpp before any pcf8574at_* function is called.
//
// Fault tolerance: the driver tracks a shadow copy of the last
// successfully-written output byte and uses that (never a live bus read) as
// the base for single-pin read-modify-write updates. A transient I2C read
// failure must never be allowed to seed a write — an all-1s/0s fallback value
// written back over the bus would clobber every OTHER output pin's state,
// not just the one being changed.

// Default I2C address when A0=A1=A2=VCC
#define PCF8574AT_ADDR_DEFAULT  0x3F

// Initialize the PCF8574AT. Sets all outputs LOW (relays off).
// `addr` is the 7-bit I2C address (e.g. 0x3F).
// Returns false if the initial bus write failed (wrong address, bus
// contention, missing pull-ups) — the driver stays disabled until the next
// pcf8574at_init() call.
bool pcf8574at_init(uint8_t addr);

// Write all 8 output states at once. `value` bit 0 = P0, bit 7 = P7.
// Returns false if the I2C write failed; the shadow byte is only updated on
// success, so a failed write is never silently treated as applied.
bool pcf8574at_write_byte(uint8_t value);

// Set a single output pin (0-7) to the given state.
// Read-modify-write against the in-driver shadow byte (last confirmed-good
// write), never a live bus read — see the fault-tolerance note above.
// Returns false if the write failed (or pin/init state was invalid), in
// which case the shadow byte is left unchanged.
bool pcf8574at_set_pin(uint8_t pin, bool state);

// Read the current state of all 8 pins directly off the bus. Returns a byte
// where bit N = pin PN. Diagnostic/input-mode use only — never used as the
// basis for a write (see pcf8574at_set_pin).
uint8_t pcf8574at_read();

#endif // PCF8574AT_H
```

- [ ] **Step 2: Update the implementation**

Replace the full contents of `src/pcf8574at.cpp` with:

```cpp
#include "pcf8574at.h"
#include <Arduino.h>
#include <Wire.h>

// ── Static state ────────────────────────────────────────────────────────────

static uint8_t  g_addr = PCF8574AT_ADDR_DEFAULT;
static bool     g_inited = false;
static uint8_t  g_shadow = 0x00;  // last confirmed-good output byte

// ── Public API ──────────────────────────────────────────────────────────────

bool pcf8574at_init(uint8_t addr) {
    g_addr = addr;
    g_shadow = 0x00;

    // Set all outputs LOW (relays off on boot). The PCF8574AT powers on with
    // all pins HIGH (weak pull-up), so we must actively drive them LOW.
    Wire.beginTransmission(g_addr);
    Wire.write(g_shadow);
    uint8_t err = Wire.endTransmission();
    g_inited = (err == 0);
    return g_inited;
}

bool pcf8574at_write_byte(uint8_t value) {
    if (!g_inited) return false;
    Wire.beginTransmission(g_addr);
    Wire.write(value);
    uint8_t err = Wire.endTransmission();
    if (err != 0) return false;
    g_shadow = value;
    return true;
}

bool pcf8574at_set_pin(uint8_t pin, bool state) {
    if (!g_inited || pin > 7) return false;

    // Read-modify-write against the shadow byte — NOT a live bus read. A
    // transient I2C read failure must never seed a write: it would clobber
    // every other output pin, not just this one.
    uint8_t next = g_shadow;
    if (state) {
        next |= (1 << pin);
    } else {
        next &= ~(1 << pin);
    }
    return pcf8574at_write_byte(next);
}

uint8_t pcf8574at_read() {
    if (!g_inited) return 0xFF;

    Wire.requestFrom(g_addr, (uint8_t)1);
    if (Wire.available()) {
        return Wire.read();
    }
    return 0xFF;  // bus error — return all-high (safe: relays off if active-LOW)
}
```

- [ ] **Step 3: Rebuild**

```bash
cd sim && make clean >/dev/null && make all
```

Expected: builds clean (`pcf8574at.cpp` is compiled directly into the sim binary per `sim/Makefile`).

```bash
cd /home/sayem/sources/power-monitoring && ~/.platformio/venv/bin/pio run -e esp32dev -e esp32c3 -e esp32c3_nodisplay
```

Expected: `3 succeeded`. Note: this will build and link fine even before Task 6 — `switch_controller.cpp` currently calls `pcf8574at_set_pin()`/`pcf8574at_init()` as statements, and discarding a `bool` return value is legal C++, not a compile error. Task 6 isn't required for the build to pass; it's required so the new return values are actually acted on (logged) instead of silently discarded, which is the point of the fix. Don't commit yet — see Step 4.

- [ ] **Step 4: Commit** (hold off — combine with Task 6's commit below, so the driver fix and the caller-side logging that makes it observable land as one logical change)

---

### Task 6: Update `switch_controller.cpp` for the new return values

**Files:**
- Modify: `src/switch_controller.cpp`

- [ ] **Step 1: Check `pcf8574at_set_pin()`'s return in `set_switch_pin()`**

Find:

```cpp
static void set_switch_pin(const SwitchChannel& ch, bool energized) {
    // I2C I/O expander path (PCF8574AT): gpio_pin stores the expander bit 0-7
    if (ch.type == SW_EXPANDER) {
        if (ch.gpio_pin > 7) {
            LOG_PRINT("[SWITCH] PCF8574AT pin %d out of range (0-7)\n", (int)ch.gpio_pin);
            return;
        }
        bool pin_high = ch.active_high ? energized : !energized;
        pcf8574at_set_pin(ch.gpio_pin, pin_high);
        return;
    }
```

Replace with:

```cpp
static void set_switch_pin(const SwitchChannel& ch, bool energized) {
    // I2C I/O expander path (PCF8574AT): gpio_pin stores the expander bit 0-7
    if (ch.type == SW_EXPANDER) {
        if (ch.gpio_pin > 7) {
            LOG_PRINT("[SWITCH] PCF8574AT pin %d out of range (0-7)\n", (int)ch.gpio_pin);
            return;
        }
        bool pin_high = ch.active_high ? energized : !energized;
        if (!pcf8574at_set_pin(ch.gpio_pin, pin_high)) {
            // One-shot-per-failure log so a stuck I2C bus doesn't drown the
            // serial console — matches the out-of-range-GPIO logging
            // convention used in the direct-GPIO path below. The commanded
            // switch_states[] entry still flips even though the physical
            // write failed; this log is the operator's only signal that the
            // relay may not actually be in the reported state.
            LOG_PRINT("[SWITCH] PCF8574AT write failed for pin %d (idx=%u) — relay state may not match commanded state\n",
                      (int)ch.gpio_pin, (unsigned)ch.idx);
        }
        return;
    }
```

- [ ] **Step 2: Check `pcf8574at_init()`'s return in `init_switches()`**

Find:

```cpp
    if (has_expander) {
        pcf8574at_init(PCF8574AT_ADDR);
        LOG_PRINT("[SWITCH] PCF8574AT initialized at 0x%02X\n", PCF8574AT_ADDR);
    }
```

Replace with:

```cpp
    if (has_expander) {
        if (pcf8574at_init(PCF8574AT_ADDR)) {
            LOG_PRINT("[SWITCH] PCF8574AT initialized at 0x%02X\n", PCF8574AT_ADDR);
        } else {
            LOG_PRINT("[SWITCH] PCF8574AT init FAILED at 0x%02X — expander-type switches will not respond until next boot\n",
                      PCF8574AT_ADDR);
        }
    }
```

- [ ] **Step 3: Check `pcf8574at_set_pin()`'s return for the boot safe-state write**

Find:

```cpp
                if (ch.type == SW_EXPANDER) {
                    // I2C expander path: no pinMode/digitalWrite needed.
                    // PCF8574AT was initialized above. Drive all expander
                    // outputs OFF on boot (safe state).
                    if (ch.gpio_pin <= 7) {
                        pcf8574at_set_pin(ch.gpio_pin, ch.active_high ? false : true);
                    }
                } else if (!gpio_in_range(ch.gpio_pin)) {
```

Replace with:

```cpp
                if (ch.type == SW_EXPANDER) {
                    // I2C expander path: no pinMode/digitalWrite needed.
                    // PCF8574AT was initialized above. Drive all expander
                    // outputs OFF on boot (safe state).
                    if (ch.gpio_pin <= 7) {
                        if (!pcf8574at_set_pin(ch.gpio_pin, ch.active_high ? false : true)) {
                            LOG_PRINT("[SWITCH] PCF8574AT boot safe-state write failed for pin %d (idx=%u)\n",
                                      (int)ch.gpio_pin, (unsigned)i);
                        }
                    }
                } else if (!gpio_in_range(ch.gpio_pin)) {
```

- [ ] **Step 4: Rebuild and run the full verification matrix**

```bash
cd sim && make clean >/dev/null && make all && make test_cycle_counter && make test_publish_path && make test_data_logger && make test_bl0939_crc
```

Expected: sim binary builds; all four suites report `N/N tests passed, 0 failed` (48, 103, 8, 4).

```bash
cd /home/sayem/sources/power-monitoring && ~/.platformio/venv/bin/pio run -e esp32dev -e esp32c3 -e esp32c3_nodisplay
```

Expected: `3 succeeded`, RAM/flash usage roughly unchanged from the pre-change baseline (esp32c3: RAM ~29.7%, Flash ~66.8%).

- [ ] **Step 5: Commit (Task 5 + 6 together — the driver signature change and its only caller must land atomically)**

```bash
git add include/pcf8574at.h src/pcf8574at.cpp src/switch_controller.cpp
git commit -m "$(cat <<'EOF'
fix(firmware): PCF8574AT shadow-state bug can force-energize unrelated relays

pcf8574at_set_pin() built its read-modify-write from a live I2C read, and
pcf8574at_read() returns 0xFF on a bus error. A transient glitch while
toggling one relay would seed the write with 0xFF, force-setting every
other expander output HIGH. Track a shadow byte updated only on confirmed-
successful writes and use that for the RMW instead. write_byte/set_pin/init
now return bool so a failed physical write is never silently reported as
applied; switch_controller.cpp logs (one-shot style) when a commanded
relay state may not match reality.
EOF
)"
```

---

### Task 7: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Full clean rebuild of everything touched**

```bash
cd /home/sayem/sources/power-monitoring/sim && make clean >/dev/null && make all && make test_cycle_counter && make test_publish_path && make test_data_logger && make test_bl0939_crc
```

Expected: sim binary + all four suites pass (48/48, 103/103, 8/8, 4/4 — same counts as the pre-change baseline recorded during the audit).

- [ ] **Step 2: Full PlatformIO rebuild, all three boards**

```bash
cd /home/sayem/sources/power-monitoring && ~/.platformio/venv/bin/pio run -e esp32dev -e esp32c3 -e esp32c3_nodisplay
```

Expected: `3 succeeded`.

- [ ] **Step 3: Confirm no orphaned declarations remain**

```bash
grep -rn "publish_data_supabase\|publish_log_batch_supabase\|flush_log_batch\|send_log_entry\b" src/*.cpp include/*.h
```

Expected: no matches.

- [ ] **Step 4: Review the full diff before considering this done**

```bash
git log --oneline -6
git diff master --stat  # or the appropriate base ref if working on a branch
```

Confirm the diff only touches the files listed in this plan's task headers, plus the two spec/plan docs already committed.
