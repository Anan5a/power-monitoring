# Finish Backend-Migration WIP + PCF8574AT Fault-Tolerance Fix

**Date:** 2026-08-10
**Status:** Approved
**Backend:** Go API (self-hosted, `backend/cmd/api`), MQTT ingest (`backend/cmd/ingest`)

---

## 1. Overview

A large uncommitted change is already in the working tree: firmware telemetry moved from Supabase REST to a flat MQTT payload matching the new Go backend's `ingest.go`, and a PCF8574AT I2C expander driver was added so `switch_controller.cpp` can drive relay outputs through an I2C expander instead of only bare GPIOs. It builds clean on all three board envs and all sim tests pass.

Auditing the diff surfaced two problems that go beyond "leftover dead code":

- Dropping the call to `check_settings_commands()` from `networkTask` removes the **only** channel by which the cloud can push config to a device (`set_relay`, `set_switch`, calibration, factory reset, …). There is no MQTT subscribe/callback anywhere in `connectivity_manager.cpp` to replace it — it was disconnected, not migrated.
- The new `pcf8574at.cpp` driver has a fault-tolerance bug: on a transient I2C read error it silently substitutes `0xFF` into the middle of a read-modify-write, which force-sets every *other* expander output HIGH — i.e. a single glitched I2C transaction while toggling one relay can energize unrelated relays.

This spec covers finishing the migration correctly and fixing the driver bug, scoped to the files already touched by the in-flight diff (plus the one new driver file). It does not touch the web frontend files (`index.html`, `pages/`, etc.) also sitting untracked in the tree — those are unrelated work.

---

## 2. Dead code removal

Confirmed via full-repo grep — no other callers exist for any of the following:

| Function | File | Why it's safe to delete |
|---|---|---|
| `publish_data_supabase()` (both overloads) | `connectivity_manager.cpp` | Fully redundant — `publish_data()` already publishes the equivalent MQTT payload to the new backend. |
| `publish_log_batch_supabase()` | `connectivity_manager.cpp` | Supabase transport for the RAM/LittleFS delta-log ring buffer. SD-card daily CSV logging (`data_logger.cpp`, already committed) is the durable local store now — confirmed with the user, no cloud backfill replacement needed. |
| `flush_log_batch()`, `send_log_entry()` | `connectivity_manager.cpp` | Only called from `publish_log_batch_supabase()`; dead once it's removed. |

Also remove the corresponding declarations from `connectivity_manager.h`.

**Not touched:** `get_ble_pin_from_supabase()`, `sync_device_channels_to_supabase()`, `sync_calibration_to_supabase()`, `sync_ble_pin_to_supabase()`, `publish_switch_state()` — these are still called from `loop_connectivity()`'s deferred-request queue and BLE handlers. Out of scope.

**Noted but out of scope:** `publish_log_batch()` (MQTT-based, `connectivity_manager.cpp:738`) already exists and is a pre-existing, independent gap — it's declared in the header but never called from anywhere, including before this diff. Not part of this cleanup; flagging only so it isn't mistaken for something this pass fixed.

---

## 3. Port `check_settings_commands()` to the new backend

Replace the Supabase RPC call (`POST {supabase_url}/rest/v1/rpc/claim_settings_command`) with the backend's existing command-queue endpoints:

- `GET {backend_url}/commands/{device_key}/pending` — atomically claims and returns an array of `{id, device_key, cmd_type, payload, status, created_at}`. (Old path handled exactly one command per poll; new path may return several — loop over the array.)
- `POST {backend_url}/commands/{id}/result` with body `{"status": "applied"|"failed", "result"?: {...}, "error"?: string}` — the old Supabase path never reported results; this is new behavior the backend expects.

Auth: both endpoints go through `DeviceAuthMiddleware`, which accepts `X-Device-Key` / `X-Api-Key` headers. Reuse `settings_load_supabase_device_key()` / `settings_load_supabase_api_key()` (same NVS-backed credentials already used for MQTT auth in `connect_mqtt()` — the "supabase" naming is legacy but the values are the shared device credentials) and `settings_load_ota_backend_url()` for the base URL, matching the pattern in `ota_client.cpp`'s `OTA_CHECKING` state (local `WiFiClientSecure` + `HTTPClient`, `.setInsecure()`, `.setHandshakeTimeout(10)`, `StaticJsonDocument` parse, checked return codes).

Flow per poll (every 5s, same cadence as before):
1. Skip if `backend_url`, `device_key`, or `api_key` aren't configured, or free heap is below `MIN_FREE_HEAP_FOR_PUBLISH`.
2. `GET /commands/{device_key}/pending` with the two headers.
3. On 200, parse the JSON array. For each command: call the existing `apply_settings_command(cmd_type, payload)` / `apply_settings_posthook(cmd_type)` (unchanged — this logic doesn't care where the command came from).
4. `POST /commands/{id}/result` with `status: applied` on success, `status: failed` + `error` on failure.

Re-wire the call back into `networkTask` in `main.cpp` at the same site + cadence it was removed from.

---

## 4. Stale comment fix

`main.cpp`, the block comment above `networkTask`, currently reads "Handles: MQTT loop, HTTP publish, Supabase telemetry + settings poll + OLED display." Update to reflect MQTT telemetry + backend command poll + OLED display — no more Supabase telemetry/settings poll.

---

## 5. PCF8574AT driver: shadow-state fix

**Bug:** `pcf8574at_set_pin()` calls `pcf8574at_read()` to get the current byte for its read-modify-write. `pcf8574at_read()` returns `0xFF` whenever the I2C read fails (bus error, device not responding) — that `0xFF` then becomes the RMW base and gets written back, forcing every bit HIGH regardless of prior state. One transient glitch while setting pin 3 can unintentionally energize pins 0,1,2,4,5,6,7.

**Fix:**
- Add an in-driver shadow byte (`static uint8_t g_shadow`), initialized to `0x00` in `pcf8574at_init()` (matches the existing "all outputs LOW on boot" behavior) and updated only after a **confirmed successful** write.
- `pcf8574at_set_pin()` computes its RMW from `g_shadow`, never from a live bus read. `pcf8574at_read()` remains available for diagnostic/input-mode use but is no longer in the write path.
- `pcf8574at_write_byte()` and `pcf8574at_set_pin()` return `bool` (success/failure) instead of `void`, so callers can detect a failed physical write instead of silently assuming it applied.
- `switch_controller.cpp`'s `set_switch_pin()` checks that return value and logs on failure via the existing one-shot-log convention already used for out-of-range GPIOs in the same function — consistent with how MQTT publish failures are already checked and logged elsewhere in this codebase.

---

## 6. Testing

- `sim/` compiles the real `pcf8574at.cpp` against `sim/hal/wire_stub.cpp`. The shadow-state fix is a pure logic change with no host-side test coverage today (no I2C failure-injection in `wire_stub.cpp`); verified by code inspection plus the existing sim build/test suite passing, not by a new failure-injection test. Adding wire-stub fault injection is a reasonable follow-up but is not required for this pass.
- Re-run full sim suite (`test_cycle_counter`, `test_publish_path`, `test_data_logger`, `test_bl0939_crc`) after each change.
- Rebuild all three PlatformIO envs (`esp32dev`, `esp32c3`, `esp32c3_nodisplay`) after each change.
- No new sim test is added specifically for the backend command-poll HTTP flow — the sim harness doesn't model backend HTTP command responses today (only the old Supabase claim shape was ever exercised, and that coverage was already thin). Manual verification against the real backend is out of scope for this pass since it requires a running backend instance; flagged as a residual gap.

---

## 7. Out of scope

- Full removal of Supabase from `settings_manager.cpp`, BLE commands, serial CLI, and docs — those still reference "supabase_*" NVS keys/settings that are reused as the generic device credential store; renaming/removing them is a separate, much larger blast-radius change.
- `publish_log_batch()` (the orphaned MQTT log-batch function) — pre-existing gap, not introduced by this diff.
- The web frontend files untracked in the working tree (`index.html`, `landing.html`, `login.html`, `signup.html`, `pages/`, `figma-shots/`, `apply_theme.py`) — unrelated work sitting in the same tree.
- `bl0939_pod.cpp`'s tautological `BL0939_COUNT` comparison warning — pre-existing, harmless (BL0939 is disabled on all boards; `ENABLE_BL0939=0`), not part of this diff.
