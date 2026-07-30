# Firmware Analysis, Improvement & Enhancement Plan

A fresh, code-grounded audit of the ESP32 power-monitoring firmware and a
prioritized plan to fix what's broken and extend what's missing. Findings were
produced by reading the current source end-to-end (no reliance on prior audits)
and then spot-verifying the highest-impact claims against the code.

## 1. Executive summary

The firmware is architecturally sound — a clean pod/channel sensor model, a
three-task FreeRTOS design, NVS-persisted settings, delta-compressed logging,
and a rich BLE provisioning surface. However, the audit surfaced a set of
**real, mostly silent failure modes** that fall into three themes:

1. **The device no longer matches the cloud backend.** MQTT publishes to a
   hardcoded `power-monitor/data` with a shared client ID and no credentials;
   the backend (just rebuilt) expects `telemetry/{device_type}/{device_key}`,
   `status/{device_key}/online`, and `device_key`/`api_key` auth. The two sides
   will not interoperate until the connectivity layer is realigned.
2. **Safety-critical relay behavior is fragile.** Auto-trip rules are silently
   disabled after every reboot; a rule blob is written to NVS flash ~8×/second
   per switch (flash-wear); the pulse path blocks and races the auto path; AND
   rules with a disabled condition can never trip.
3. **Data-loss and data-corruption hazards in persistence/logging.** A
   successful telemetry RPC deletes the undrained LittleFS overflow backlog;
   the delta-log `dt` is truncated to 16 bits before its 60 s threshold check;
   factory reset wipes the wrong NVS namespace (battery data survives);
   calibration migration writes legacy bytes into the wrong fields; LittleFS
   I/O runs inside a spinlock.

There are also clear enhancement opportunities — online/offline heartbeat,
OTA-over-MQTT, sensor health diagnostics, secure BLE pairing, a unified
command table, voltage-fusion SoC — detailed in §6.

This document is the plan; per-issue code locations are cited so each item is
directly actionable.

## 2. Methodology

- Read `platformio.ini`, `include/config.h`, all `include/boards/*.h`,
  `src/main.cpp`, and `docs/ARCHITECTURE.md` for the structural model.
- Deep-read every `.cpp`/`.h` across five subsystem clusters (connectivity,
  sensors, BLE, switch/UI, persistence/counters).
- Verified the five most surprising CRITICAL claims against the source
  (BLE PIN logging, per-tick NVS rule save, hardcoded MQTT topic/client ID,
  factory-reset namespace, delta-log `dt` truncation) — all confirmed.
- Findings are ranked by real-world impact for an unattended, mains-powered
  device controlling relays.

## 3. Current architecture (recap)

- **Tasks**: `networkTask` (core 0, 10 ms — WiFi/MQTT/Supabase/BLE/display),
  `sensorTask` (core 1, 1 s — I2C reads, logging, counters, switch eval),
  `uiTask` (core 1, 50 ms — buttons/LEDs), `loop()` (serial CLI). 30 s TWDT on
  the three real tasks.
- **Sensor model**: pods (INA226 I2C, BL0939 UART) → `SensorSnapshot` with
  flat logical-channel indexing; accessor functions with snapshot and
  non-snapshot overloads; `g_sensor_queue` core1→core0.
- **Persistence**: `Preferences` (NVS) for settings/counters; 16 KB RAM
  delta-compressed ring buffer with LittleFS overflow for logs.
- **Cloud**: MQTT (PubSubClient) + optional HTTP endpoint + Supabase RPC
  (telemetry insert, log-bin drain, settings/command poll, calibration sync).
- **Control**: rule-based switch/relay auto trip/reset (delay, hysteresis,
  multi-condition, schedule), manual override, pulse; 5-page OLED; BLE GATT
  command server (NimBLE) with a numeric PIN.

## 4. Analysis by subsystem

### 4.1 Connectivity & telemetry (`connectivity_manager.cpp/.h`, `telemetry.cpp/.h`)

**Critical**
- MQTT topic, client ID, and auth do not match the backend contract.
  `connectivity_manager.cpp:462-463,1227` — publishes to `MQTT_TOPIC`
  (`"power-monitor/data"`, `config.h:25`), LWT on `"power-monitor/data/status"`,
  client ID literal `"power-monitor-esp32"`, `mqtt.connect()` with **no
  username/password**. The device-specific `mqtt_topic` loaded from NVS
  (`:562`) is read and discarded. Two devices on one broker collide on the
  client ID and kick each other off. (Backend expects `telemetry/{type}/{key}`,
  `status/{key}/online`, `device_key`/`api_key`.)
- A successful telemetry RPC deletes the LittleFS overflow backlog.
  `connectivity_manager.cpp:1322-1324` calls `log_close_overflow()` on a 200/204
  from `insert_telemetry`, but that RPC carries a *live snapshot*, not the
  spilled backlog. `log_close_overflow()` removes `LOG_OVERFLOW_FILE`
  (`data_logger.cpp:395-401`). Undrained offline data is destroyed the moment
  the device reconnects. **Data loss.**

**High**
- Blocking TLS handshake (30 s) + NTP (5 s) + Supabase calibration sync on the
  network task can exceed the 30 s TWDT → panic/reboot loop, especially on
  first boot with an unreachable server (`connectivity_manager.cpp:121,166,
  296, 409-423`). The `WST_CONNECTING→CONNECTED` transition the comments claim
  is non-blocking is actually synchronous for ~8-15 s.
- Capacity-test SoH one-shot side channel is consumed by the first
  `telemetry_build()` of each cycle (`telemetry.cpp:217-223`); `main.cpp:115-116`
  calls `publish_data()` then `publish_data_supabase()` back-to-back, so the
  Supabase snapshot (the one that needs it) never gets `capacity_test_soh_valid`.
- `publish_data_http` cannot do HTTPS: `http.begin(url)` (single-arg) doesn't
  support TLS on ESP32 Arduino (`:508-512`); an https custom endpoint silently
  fails. No `http.setTimeout()`.

**Medium**
- `decode_and_send_log_entries` advances only 1 byte on a `DeltaEntry` with no
  preceding base (`:1003`), corrupting the stream. Should `break`.
- `flush_log_batch` stamps 1970 timestamps when NTP was invalid (`:898,964`).
- `publish_log_batch` pops entries from the ring *before* `mqtt.publish()`; on
  broker failure the popped entries are gone (`:627-637,642-651`). **Data loss
  on transient outage.**
- Telemetry JSON > `MQTT_MAX_PACKET_SIZE` (2048) or > `TELEMETRY_BUF_BYTES`
  (4096) is silently dropped at saturation (`:1207-1213,1227`).
- BLE notify fallback sends the full multi-KB JSON over a 20-byte MTU (`:1244`).
- `apply_settings_posthook("set_wifi")` calls `WiFi.begin(...)` without
  resetting the state machine (`:1581`); the bounded connect/re-sync path
  doesn't run for the new SSID.
- Re-enabling BLE after a WiFi drop is broken: BLE was `deinit`'d at
  `WST_CONNECTING` but `start_ble_advertising` no-ops when uninitialized
  (`:552-556` vs `ble_provisioner.cpp:1335`).

**Smells**: settings re-read from NVS every 10 ms tick (`:562,615`);
`supabase_http_prepare` always tears down so `g_supa_http_ready` is dead
(`:153-158`); two `telemetry_build()` calls per cycle (`main.cpp:115-116`);
five inconsistent heap-threshold magic numbers (`:72-75`); low-freq Supabase
paths forcibly reset the high-freq telemetry TLS client (`:1469,1511,...`).

### 4.2 Sensor layer (`sensor_manager.cpp/.h`, `sensor_pod.h`, `bl0939_pod.cpp/.h`, `serial1_manager.cpp/.h`)

**High**
- Spike/stddev is computed but never exposed: `sensor_get_meta()` returns
  zeros because no pod driver writes `PhysicalChannel::meta`; INA3221 writes
  to a *global* `g_meta[]` instead (`:150,475-479`). The spike-detection
  feature is inert from any caller's perspective.
- INA226 discovery cap (`MAX_INA226=8`) exceeds the read cap (`INA226_COUNT=4`).
  `discover_ina226()` registers up to 8 pods (`:259`) but `pod_ina226_read`
  returns zeros for `dev_idx >= 4` (`:183`). Pods 4-7 are registered, counted,
  published, and always read zero.
- BL0939 is never wired in: `register_pod(POD_BL0939,...)` is never called and
  `bl0939_pod_init()` is never invoked from `main.cpp`. The driver + parser
  exist but AC readings never flow. `BL0939_COUNT=0` on all boards would reject
  every frame even if it were.

**Medium**
- Cached INA226 restore desyncs `ina226_device_for_pod` when any device fails
  `begin()` (`:302-325`) — the working device is orphaned.
- INA226 power is not recomputed after V/I calibration: `ch->power = p` from
  raw `getPower()` while V and I are calibrated (`:190-192`). `P ≠ V·I`.
- Non-snapshot `get_channel_*()` accessors return pointers into live `g_pods`
  mutated every 1 s; many callers read V then I then P separately → torn reads
  (`:458-469`; callers in `main.cpp:47,54,379,426,461`,
  `connectivity_manager.cpp:250-263,1156-1197`).
- `discover_sensors()` silently drops the INA3221 legacy fallback after
  `clear_pods()` (`:392-393,362-371`).
- I2C discovery probe writes config registers to non-INA226 devices that ACK
  in the scan range (e.g. ADS1115 at 0x48) before `isConnected()` rejects
  (`:216-219`).
- BL0939 UART uses shared static state and doesn't route frames by address to
  pods (`bl0939_pod.cpp:77-98`); with multiple meters, pod A's frames land in
  pod B.
- Hardcoded "channel 3 == INA226" assumption (`main.cpp:54,443`,
  `connectivity_manager.cpp:250-263`) breaks when fewer than 4 INA226s are
  discovered — silently returns 0.0.

**Low**: baseline/stddev only covers INA3221's first 3 channels; BL0939 sync
has no idle timeout/mis-lock risk; `max_current` unbounded upward for tiny
shunts; `sensor_get_calibration` masks out-of-range `ch` to channel 0;
`serial1_read_line(len=0)` underflows `size_t`; `ina226_getShuntVoltage()`
reports only the first device.

**Smells**: dead INA3221 legacy path retained; hardcoded `3`/`4` channel
counts vs `MAX_LOGICAL_CHANNELS`; `ina226_addresses[]` declared unused;
`SPIKE_DEVIATION_MV` unused; two parallel channel-accessor APIs.

### 4.3 BLE provisioning (`ble_provisioner.cpp/.h`)

**Critical**
- The BLE PIN is logged to the serial console on every command.
  `ble_provisioner.cpp:130` logs the full raw command JSON (which includes
  `"pin":"123456"` on every PIN-protected command); `:140-142` explicitly log
  the provided PIN, the stored PIN, and the expected value. The device's only
  secret is written to serial on every write.
- `default_switch_pins[4]` is indexed by `idx` validated only as `<=7`; for
  `idx ∈ [4,7]` with `gpio_pin` omitted, `doc["gpio_pin"] | default_switch_pins[idx]`
  reads 1-4 elements past the end of the 4-entry array → stack garbage becomes
  the default GPIO and may be persisted to NVS (`:163,168`).
- The rate limiter is bypassable: `check_rate_limit()` only counts commands
  within a 100 ms window (`:109-127`); one command every ~100 ms never trips
  the limit → ~10 PIN guesses/sec, no persistent failed-PIN counter, no
  backoff. 6-digit PIN exhaustible in well under 28 h.
- `onWrite` runs in the NimBLE host task and synchronously mutates state
  shared with the sensor/network tasks (NVS writes, `sensor_calibrate_baseline`,
  `reset_coulomb_counter`, `cycle_counter_reset`, `apply_settings_posthook`
  which triggers WiFi/MQTT reconnect) with no lock (`:58-72,144-145,...`).
  NVS reentrancy, counter-reset races, and BLE host-task blocking.

**High**
- Duplicate `get_battery`/`get_battery_profile` branches make the v2
  id/chemistry-based getters unreachable dead code (`:427/800`, `:595/765`).
  `set_battery_profile v:2` can create profiles the matching getter can't
  retrieve.
- BLE `set_supabase` unconditionally writes `anon_key`/`api_key`/`device_key`
  via `settings_save_supabase_*(... | "")` (`:494-497`), clobbering existing
  secrets whenever a field is omitted — partial updates brick Supabase. The
  Supabase path (`:977-985`) guards correctly; the BLE path diverged.
- Characteristics use plain `WRITE`/`READ`/`NOTIFY` with no encryption/auth and
  no `NimBLEDevice::setSecurityAuth`/bonding (`:1302-1321`). The PIN travels
  in plaintext over an unencrypted link; anyone in range can connect subject
  only to the weak PIN gate.
- `factory_reset`/`reboot`/`set_pin` are reachable over the Supabase command
  channel with no device-side verification (`:1256,1277-1287,1302-1321`); the
  code's own TODO acknowledges an attacker who can insert a command row can
  wipe the device or change the PIN and lock out the owner.

**Medium**: `get_status` has no PIN check (`:360-369`); channel index
unvalidated for `set_shunt`/`set_volt_ratio`/`set_resistors`/`reset_coulomb`/
`set_channel_name`/`get_channel_name`/`reset_battery` (`:327,333,339,372,...`);
`PIN == 0` disables all auth (`:94`); `send_error` builds JSON with
attacker-controlled `cmd` unescaped → JSON injection (`:85-88,136`);
`set_http` silently enables HTTP when `enabled` omitted (`:156`);
`get_supabase` returns `api_key`/`device_key` in plaintext (`:500-520`);
`capacity_test` doesn't validate `mode`/`load_switch_idx` (`:846-847`); PIN
compare non-constant-time (`:102`).

**Smells**: `handle_command` is a ~775-line dispatch (`:138-913`);
`apply_settings_command` duplicates ~360 lines of dispatch and has already
diverged from it (`:930-1292`); inconsistent error-response shape; inconsistent
channel validation; `apply_settings_posthook` invoked for some settings and
not others with no stated reason; magic buffer sizes.

### 4.4 Switch/relay control & UI (`switch_controller.cpp/.h`, `ui_manager.cpp/.h`, `display_manager.cpp/.h`)

**Critical**
- `evaluate_switches` writes the full 148-byte `SwitchRule` blob to NVS **every
  1 Hz tick for every switch**, unconditionally (`switch_controller.cpp:493`).
  With 8 switches that's ~8 flash page writes/second (~250 M writes/year)
  against ~100 k erase endurance — the device will wear its flash out in weeks
  to months. It's also a read-modify-write race with BLE/serial rule edits
  between `settings_load_switch_rule` (`:405`) and `settings_save_switch_rule`
  (`:493`).
- `switch_pulse` blocks its calling task (up to 3000 ms) and races the auto
  path: it doesn't check/disable `switch_auto_enabled`, so `evaluate_switches`
  (1 Hz) can overwrite the pulse mid-flight via the same `switch_states[]`/NVS/
  GPIO (`switch_controller.cpp:541-549`). "test all switches" blocks the main
  loop for ~12 s (`main.cpp:746-752`).

**High**
- `switch_auto_enabled` is RAM-only and defaults to `false` on every reboot
  (`:54,321`). A device whose job is overcurrent/undervoltage/SoC protection
  silently has **all safety rules disabled** after any reboot (power loss,
  OTA, crash, WDT) until a human sends "switch auto on". No boot warning, no
  NVS option to persist/auto-enable.
- `init_switches` restores `is_energized` from NVS and drives the pin HIGH on
  boot if it was energized at shutdown (`:375-376,389-391`). Combined with
  auto-disabled-on-boot, a load that was ON before a reboot comes back ON and
  stays ON with no rule evaluation. No safe-state-on-boot interlock.
- UI long-press handler fires every 50 ms while a button is held past 5 s
  (`ui_manager.cpp:191-198`); for button 2 this re-issues `discover_sensors()`
  every 50 ms for the whole hold. On release it fires a second time.
- Button 0 short-press page-cycle is dead: `ui_next_display_page()` is never
  called by `display_manager.cpp` or `main.cpp` (`ui_manager.cpp:206-212`).
- AND rule with any DISABLED condition is permanently un-trippable
  (`:441-442,414`): `true_count >= condition_count`, but disabled conditions
  stay false and still count toward the denominator.

**Medium**: no mutex around `switch_states[]`/NVS/GPIO across UI/BLE/serial/
sensor tasks (`:529-539`); `get_switch_state` reads raw GPIO instead of the
authoritative `switch_states[].energized` (`:551-557`); `SCO_EQ` uses a
milliwatt epsilon on an ampere value (`:236-241`); `eval_schedule` relies on
`localtime` with no timezone configured (`:250-265`); `init_display`
re-inits `Wire` independently of `sensor_manager` and the "test display" CLI
calls `update_display` from the main loop task → cross-task I2C with no mutex
(`display_manager.cpp:203-207`, `main.cpp:759-763`); `switch_set_auto(false)`
only resets switches whose `rule.enabled` is true (`:504-522`).

**Low**: initial `publish_switch_state` hardcoded to `i < 4` (`:389-391`);
`(int32_t)(now - start_ms)` wraps negative after ~24.8 days of continuous
hold (`:455,473`); UI button/LED pins not validated against the BAD_GPIO
denylist (C3 button 0 = GPIO 0, network LED = GPIO 2, both strapping/denied)
(`ui_manager.cpp:49`); dead `network_*`/`system_*` UI state vars (`:37-40`).

**Smells**: duplicate `default_pins[4]` vs `ble_provisioner.cpp:163`;
`MAX_SWITCHES=8` magic in the .cpp while `SC_MAX_CONDITIONS` is a header macro;
`SCK_CHANNEL_ABOVE/BELOW` compare current but are named "channel above";
duplicated SoC computation across switch/display; non-reentrant `static`
buffers in `draw_channel_page`; the threading comment documents a known race as
accepted design (`switch_controller.h:91-104`).

### 4.5 Persistence & counters (`data_logger.cpp/.h`, `settings_manager.cpp/.h`, `coulomb_counter.cpp`, `energy_counter.cpp`, `cycle_counter.cpp`, `capacity_test.cpp`, `battery_*.cpp/.h`)

**Critical**
- `settings_factory_reset` clears the wrong NVS namespace: it opens
  `"pm-battery"` (`settings_manager.cpp:444`), but battery state/profiles live
  in `"pm-battery-state"` / `"pm-battery-profile"` (`battery_nvs.h:15-16`).
  `prefs.clear()` on a never-used namespace is a no-op, so channel bindings,
  chemistry profiles, and per-channel cycle/capacity-test state **survive
  factory reset**. The LittleFS overflow log is also not removed.

**High**
- `ChannelCalibration` v1→v2 "migration" writes legacy bytes into the wrong
  fields (`settings_manager.cpp:197-202`): the 48-byte legacy blob (four
  `float[3]`) is copied to the front of the v2 struct, landing legacy
  `volt_gain`/`curr_offset`/`curr_gain` inside `volt_offset_mv[4..11]`. Upgraded
  devices run with corrupted calibration; the version byte is then stamped v2,
  masking it.
- Stale capacity-test recovery guard fails on fresh boot: `if (now >
  started_ms && (now - started_ms) > kStaleTestMaxMs)` (`cycle_counter.cpp:101`)
  — `started_ms` is pre-reboot uptime; on boot `now≈0`, so `now > started_ms`
  is false and a crashed test is never auto-cancelled (the exact case it was
  meant to handle). Should use wrap-safe `(uint32_t)(now - started_ms)`.
- Delta-log `dt` truncation: `uint16_t dt = (uint16_t)(timestamp_ms - last_ts);
  if (... || dt > 60000)` (`data_logger.cpp:242-243`). The cast truncates
  *before* the comparison; a 70 s real gap becomes `dt=4464`, misses the
  BaseEntry fallback, and shifts every later reconstructed timestamp by ~65.5
  s. Same failure at the 49-day `millis()` wrap.
- LittleFS flash I/O inside `taskENTER_CRITICAL` (`data_logger.cpp:267` taken;
  `:292-299` does `LittleFS.totalBytes/usedBytes`, `flush_to_littlefs` →
  exists/open/write/close/remove, all under the lock until `:318`). Flash
  erase+write can take tens of ms, blocking the other core and risking WDT
  trips. The battery subsystem explicitly forbids I/O under its lock
  (`battery_lock.h:18`); the data logger violates the same discipline.
- Counter `dt_seconds` hardcoded to `1.0f` by the caller (`main.cpp:183-186`).
  `sensorTask` uses `vTaskDelayUntil` but never computes elapsed `dt`; if the
  task slips (slow I2C, WDT reset), all four counters integrate as if exactly
  1 s → systematic energy/mAh drift.

**Medium**
- `log_peek_latest` reads `last_v/last_i/last_p/last_ts` without the lock
  (`data_logger.cpp:347-356` vs writer at `:256`) → torn reads across cores.
- `g_overflow_file` handle raced across cores without the lock (`:390-401` vs
  `:122-125`).
- `g_last_result[]` in `capacity_test` written from sensor and network tasks
  unlocked (`:169,198,223,229`).
- `cycle_counter_get → modify → put` in capacity_test is non-atomic across two
  `BATTERY_LOCK` windows (`:243-244,284,300-301`); a BLE `cycle_counter_reset`
  between them is overwritten.
- Single static `Preferences prefs` in `settings_manager` used from multiple
  tasks without a mutex (`settings_manager.cpp:5`); Arduino `Preferences`
  holds one `nvs_handle`, not reentrant.
- `update_energy_counter` does `settings_load_virtual_channel` 16×/sec uncached
  (`energy_counter.cpp:27`).
- `BatteryState` v1 blobs are rejected with no migration → upgrade loses all
  cumulative Ah/cycle history (`battery_state.cpp:31,105-108`).
- `g_last_dir[ch]` reset to 0 on lazy-load loses in-progress partial-cycle
  accounting across reboot (`cycle_counter.cpp:147,224,282`).
- Coulomb SoC ignores `BatteryConfig.initial_soc_pct` and assumes 100% at boot
  (`coulomb_counter.cpp:20-22`, `cycle_counter.cpp:73`) — a battery installed
  at 50% reads 100% for its whole life.
- Automated capacity-test cutoff fires on a single `v < cutoff_v` sample, no
  debounce (`capacity_test.cpp:265-272`) — an inrush dip ends the test.

**Low**: `log_peek_latest` ignores `g_log_epoch_valid`; overflow file may never
be drained (no verified caller); `battery_state_reset` writes then removes;
`battery_profile_list_ids` returns sparse/deleted slots; `battery_profile_set`
no semantic validation; Ah-in/out deadzone (0.02 A) disagrees with coulomb
SoC; no graceful-shutdown persistence (≤5 min loss); inconsistent snapshot
accessor usage across counters.

**Smells**: duplicated 300000 ms persist interval across four counters;
size-only backward compat for `RelayRule`/`SwitchChannel`/`SwitchRule` with no
version byte; `BatteryState`/`BatteryChemistryProfile` persisted raw via
`memcpy` with no `static_assert`/`packed` (cross-build padding risk);
counter init/update/persist/reset pattern duplicated; `init_battery_states` is
a no-op next to a real `init_battery_bindings`.

## 5. Prioritized improvement plan (bugs & safety)

### P0 — Critical (fix before any field deployment)

| # | Issue | Location | Fix direction |
|---|---|---|---|
| P0-1 | MQTT topic/client-ID/auth don't match backend | `connectivity_manager.cpp:462-463,1227` | Use `device_key` as client ID + username, `api_key` as password; LWT on `status/{key}/online` payload `"offline"` QoS1 retain; publish telemetry to `telemetry/{type}/{key}`; use the NVS-loaded `mqtt_topic`. (Also P0 for backend interop.) |
| P0-2 | Successful telemetry RPC deletes undrained overflow backlog | `connectivity_manager.cpp:1322-1324` | Only call `log_close_overflow()` after `publish_log_batch_supabase()` has actually drained the file, not after a live-snapshot RPC. |
| P0-3 | BLE PIN logged to serial | `ble_provisioner.cpp:130,140-142` | Strip the PIN from logs; add a redacting log helper so raw command JSON can't leak secrets. |
| P0-4 | `default_switch_pins[4]` OOB read for idx 4-7 | `ble_provisioner.cpp:163,168` | Clamp idx to the array size or require `gpio_pin` for idx ≥ 4. |
| P0-5 | Switch rule written to NVS every 1 Hz tick (flash wear + race) | `switch_controller.cpp:493` | Persist only when `condition_latched[]`/`is_energized` actually change (memcmp vs loaded copy). |
| P0-6 | Factory reset wipes wrong NVS namespace | `settings_manager.cpp:444` | Clear `kBatteryStateNs`/`kBatteryProfileNs` (via `battery_nvs_*` helpers) + remove `LOG_OVERFLOW_FILE`. |
| P0-7 | `ChannelCalibration` v1→v2 migration corrupts calibration | `settings_manager.cpp:197-202` | Write a real field-by-field migrator (map legacy `float[3]` quartets to the correct v2 arrays), or reject v1 and re-default. |
| P0-8 | Delta-log `dt` truncation before threshold check | `data_logger.cpp:242-243` | Compute `uint32_t dt32 = timestamp_ms - last_ts`; force BaseEntry when `dt32 > 60000`; store `min(dt32,60000)`. |

### P1 — High

- **P1-1** Auto-trip disabled after every reboot, no boot warning, no NVS option
  (`switch_controller.cpp:54,321`). Persist `switch_auto_enabled` to NVS with
  an explicit "auto-enable on boot" config; at minimum log a loud warning.
- **P1-2** No safe-state-on-boot; restored relays energize without re-evaluating
  rules (`switch_controller.cpp:375-376,389-391`). Drive all relays OFF at init
  and require one full `evaluate_switches` pass with auto enabled before any
  rule may energize.
- **P1-3** `switch_pulse` blocks and races auto (`switch_controller.cpp:541-549`).
  Move pulse to a non-blocking timed state; have it temporarily suspend auto
  for that switch; guard with the switch mutex.
- **P1-4** AND rule with disabled conditions is un-trippable
  (`switch_controller.cpp:441-442,414`). Denominator = count of non-disabled
  conditions.
- **P1-5** BLE rate limiter bypassable, no persistent failed-PIN counter/backoff
  (`ble_provisioner.cpp:109-127`). Sliding-window/token-bucket + NVS
  failed-PIN count + exponential backoff + lockout (surviving reconnects).
- **P1-6** `onWrite` mutates shared state from the NimBLE host task with no lock
  (`ble_provisioner.cpp:58-72,...`). Forward commands to the existing
  `g_cmd_queue`/sensor task (or a dedicated command task) instead of executing
  synchronously in the BLE callback.
- **P1-7** BLE/Supabase command dispatchers diverged; `set_supabase` over BLE
  clobbers secrets (`ble_provisioner.cpp:494-497,930-1292`). Unify into one
  command table used by both paths; apply the Supabase path's partial-update
  guard to BLE.
- **P1-8** Blocking TLS/NTP + Supabase sync can trip the 30 s WDT
  (`connectivity_manager.cpp:121,166,296,409-423`). Lower
  `setHandshakeTimeout` to ~10 s, set `http.setTimeout(4000)`, move
  `sync_time`/calibration sync/first `connect_mqtt` out of the state transition
  into one-shot CONNECTED tick work.
- **P1-9** Capacity-test SoH consumed by the first `telemetry_build()`
  (`telemetry.cpp:217-223`, `main.cpp:115-116`). Build the snapshot once per
  cycle and pass it to both transports; clear the SoH flag only after the
  Supabase publish.
- **P1-10** Spike/stddev computed but never exposed; INA226 has no spike path
  (`sensor_manager.cpp:150,475-479`). Write `ch->meta` in both pod drivers;
  extend burst/stddev to INA226.
- **P1-11** INA226 discovery registers dead pods beyond `INA226_COUNT=4`
  (`sensor_manager.cpp:259,183`). Cap discovery at `INA226_COUNT`.
- **P1-12** BL0939 driver never wired in (`sensor_manager.cpp`, `main.cpp`).
  Register BL0939 pods when `BL0939_COUNT > 0`; route frames by address.
- **P1-13** Stale capacity-test recovery fails on fresh boot
  (`cycle_counter.cpp:101`). Use wrap-safe unsigned subtraction.
- **P1-14** LittleFS I/O inside the spinlock (`data_logger.cpp:267-318`). Keep
  only head/tail/`memcpy` under `LOG_LOCK`; defer flush to a task-level mutex
  or flag.
- **P1-15** Counter `dt` hardcoded to 1.0 (`main.cpp:183-186`). Compute actual
  elapsed from `vTaskDelayUntil`'s `last_wake` and pass it.
- **P1-16** `publish_log_batch` drops popped entries on broker failure
  (`connectivity_manager.cpp:627-637,642-651`). Re-insert on failure or only
  pop after a successful publish.
- **P1-17** `factory_reset`/`reboot`/`set_pin` reachable over Supabase with no
  auth (`ble_provisioner.cpp:1256,1277-1287`). Require `device_api_key`
  verification (and `old_pin` for `set_pin`) on destructive commands.
- **P1-18** UI long-press repeats every 50 ms and double-fires on release
  (`ui_manager.cpp:191-198`). Latch "long fired" for the hold duration; don't
  re-set on release.
- **P1-19** Button 0 page-cycle is dead (`ui_manager.cpp:206-212`). Wire
  `ui_next_display_page()` into `update_display`.
- **P1-20** `publish_data_http` can't do HTTPS
  (`connectivity_manager.cpp:508-512`). Pass a `WiFiClientSecure` for https
  URLs; set `http.setTimeout`.

### P2 — Medium (selected)
- ✅ INA226 power recompute after calibration — already done (`sensor_manager.cpp:198`, recomputes `ch->power = V*I` post-calibration).
- Migrate callers to snapshot accessors; deprecate non-snapshot `get_channel_*`
  to kill torn reads. *(deferred — invasive cross-module caller migration)*
- ✅ `decode_and_send_log_entries` 1-byte skip bug — delta-without-base now skips `sizeof(DeltaEntry)` (was `offset++`, desyncing the stream).
- ✅ `flush_log_batch` 1970 timestamps when NTP invalid — `p_recorded_at` now omitted when unsynced so Postgres `default now()` fills the real wall-clock (was stamping 0 → 1970 rows).
- ✅ Telemetry buffer overflow now logs `[TELEM] serialize overflow` instead of silently no-oping.
- ✅ BLE notify fallback dropped — full ~1.4 KB JSON doesn't fit the 20-byte ATT MTU; subset-overflow now logs and skips the notify instead of sending a truncated fragment.
- ✅ `apply_settings_posthook("set_wifi")` now resets `s_post_connect=PCS_IDLE` + re-enters `WST_CONNECTING` so NTP/cal/MQTT re-run on the new link.
- ✅ Re-enable-BLE-after-WiFi-drop — now calls `init_ble_provisioner()` (re-creates the NimBLE stack) instead of `start_ble_advertising()` (which no-ops after `deinit_ble_provisioner`).
- ✅ `get_status` now PIN-gated; `valid_channel()` bounds check added to key handlers; `PIN==0` now refuses all commands (must `set_pin` first); `send_error` uses ArduinoJson (no injection); `set_http` echoes `enabled` flag.
- ✅ BLE link-layer encryption + bonding (LE Secure Connections, Just Works, `WRITE_ENC` property).
- `get_switch_state` should read `switch_states[].energized`, not raw GPIO
  (`switch_controller.cpp:551-557`); switch mutex around all
  state/NVS/GPIO mutation.
- `eval_schedule` timezone (`:250-265`); `SCO_EQ` epsilon units (`:236-241`).
- `init_display`/`update_display` cross-task I2C contention
  (`display_manager.cpp:203-207`, `main.cpp:759-763`) — shared bus mutex.
- `log_peek_latest`/`g_overflow_file`/`g_last_result`/`cycle_counter` RMW
  races — add locks/atomics (`data_logger.cpp`, `capacity_test.cpp`,
  `cycle_counter.cpp`).
- Static `Preferences` mutex or per-task handles (`settings_manager.cpp:5`).
- Cache virtual-channel config (`energy_counter.cpp:27`).
- `BatteryState` v1→v2 migration to preserve history (`battery_state.cpp`).
- Coulomb SoC should honor `initial_soc_pct` (`coulomb_counter.cpp:20-22`).
- Debounce automated capacity-test cutoff (`capacity_test.cpp:265-272`). *(moot — capacity_test removed in Part 3)*
- ✅ **SD card (SPI) — auto-detect, daily CSV logs, storage management**. No feature flag — always tries `SD.begin()` on boot. If present: (1) binary overflow file `/log_overflow.bin` for the MQTT/Supabase drain path (1 MB cap), (2) daily CSV files `/logs/YYYY-MM-DD.csv` with one row per sensor sample (batched, flushed every 5 s). Storage management: prunes oldest CSV files when free space drops below 5%, targets 10% free. Per-board pin defines (SD_CS/MOSI/MISO/CLK). Fault-tolerant: card presence check before every write, write error recovery, all I/O outside the spinlock.
- `BatteryState`/`BatteryChemistryProfile`: `static_assert` + packed/CRC
  (`battery_state.h`, `battery_profile.h`).

### P3 — Low (hygiene; batch when touching the file)
- Remove dead INA3221 legacy code path; unify `MAX_SWITCHES`/`MAX_*` in
  headers; dedupe `default_pins`; shared `compute_soc`; cache MQTT settings in
  statics; one 300000 ms persist constant; `drain_response` dedup; clean dead
  UI state vars; `serial1_read_line(0)` guard; `ina226_getShuntVoltage` all
  devices; `battery_profile_list_ids` skip holes; `battery_profile_set`
  semantic validation; persist-on-shutdown hook for counters; redact secrets
  from all logs.

## 6. Enhancement roadmap

### Phase 1 — Backend alignment & safety (must precede production)
1. **MQTT contract realignment** (P0-1): `telemetry/{type}/{key}`,
   `status/{key}/online`, `device_key`/`api_key` auth, retained LWT.
2. **Online/offline heartbeat** (connectivity): publish `"online"` retained
   on connect; periodic heartbeat every 30-60 s so the backend detects a
   hung-but-TCP-connected device faster than the broker keepalive.
3. **Safe-state-on-boot + persistent auto-enable** (P1-1, P1-2): relays OFF
   until rules evaluate; auto-trip survives reboot.
4. **NVS write coalescing** (P0-5) and **move LittleFS out of the spinlock**
   (P1-14).
5. **Factory reset correctness** (P0-6) and **calibration migration** (P0-7).

### Phase 2 — Robustness & data integrity
1. **Offline publish queueing**: route dropped MQTT publishes into the
   existing `data_logger` overflow path so telemetry survives broker
   outages (the RAM ring + LittleFS already exists for the log-bin path).
2. **Bounded exponential backoff** for MQTT connect (5 s→60 s capped).
3. **Non-blocking WST_CONNECTING→CONNECTED** transition (P1-8): one-shot
   tick work for NTP/Supabase/MQTT.
4. **Build telemetry once per cycle**, pass the snapshot to MQTT/HTTP/Supabase
   (P1-9) — halves work and fixes the SoH side channel.
5. **Sensor health diagnostics**: per-pod error counters (I2C NACK,
   INA226 `isConnected` fail, BL0939 checksum fail, UART overrun), exposed
   via serial/BLE/telemetry; `valid`/NaN flags in the snapshot.
6. **I2C bus reset/recovery** on repeated NACK / stuck SDA.
7. **BL0939 parity/CRC + idle-timeout**; route frames by address (P1-12).
8. **Unified BLE/Supabase command table** (P1-7) with auth tiers
   (read/mutate/destructive) and structured error responses.
9. **NVS versioning + migration** for `RelayRule`/`SwitchChannel`/`SwitchRule`/
   `BatteryState`; `static_assert` + packed + CRC on persisted blobs.
10. **Voltage-fusion SoC**: combine coulomb counting with an OCV-from-voltage
    lookup per chemistry; honor `initial_soc_pct`.
11. **Wrap-safe dt** in the delta log (P0-8) and in the counters (P1-15).

### Phase 3 — Capabilities
1. **OTA over the command channel**: `ota_start` command (URL + expected
   hash) handled in `apply_settings_command` with a streaming HTTP-in OTA
   writer; reuse the backend's OTA release + `MINIO_PUBLIC_URL`.
2. **Remote switch control via backend commands** with signed commands +
   ack + `publish_switch_state` feedback; gate remote energize when auto is
   enabled to avoid the race.
3. **Secure BLE pairing**: LE Secure Connections, passkey entry, bonding,
   `WRITE_ENC`/`WRITE_AUTHEN` on the command characteristic; authenticated
   session after first PIN (no re-transmit every command).
4. **Schema versioning + feature negotiation**: `min_server_schema` on the
   `status/online` heartbeat so firmware/backend can negotiate breaking
   changes.
5. **Rule conflict detection/reporting**: two rules on the same switch, AND
   with disabled conditions (P1-4), overlapping schedule windows — reported
   via serial/BLE/telemetry.
6. **Per-switch audit/event log**: trip/reset/force/override events with
   timestamp + triggering condition in the data logger ring (currently lost
   on reboot).
7. **Soft-start/inrush limiting** for MOSFET/SSR switch types (PWM ramp on
   energize).
8. **Display of alerts/errors + AUTO-OFF warning** (6th OLED page).
9. **`get_diag` BLE command**: free heap, RSSI, MQTT/Supabase state, BLE MTU,
   NVS free space, uptime, error counters.
10. **`poll_commands` over BLE** so a phone can pull/ack pending backend
    commands for offline devices.

### Phase 4 — Long-term
1. **MTU negotiation + chunked BLE notifications** for large payloads.
2. **Horizontal-friendly telemetry**: per-publish channel cap for MQTT,
   overflow channels via a follow-up topic; raise/justify
   `MQTT_MAX_PACKET_SIZE`.
3. **Persistent command queue** on the device (ack/retry semantics with the
   backend's `device_commands` table).
4. **Timezone configuration** for schedule rules.
5. **SoH accounting for partial start** (`measured_Ah / (rated_Ah·(1−start_SoC))`).
6. **Wear-aware counter persistence** (persist deltas, not full blobs).

## 7. Test & verification strategy
- **Sim harness** (`sim/`): extend the existing CMake/Make sim build to cover
  the delta-log `dt` truncation, calibration migration, stale-test recovery,
  and SoC math with controlled time (`sim/shims/`).
- **Hardware-in-loop smoke**: verify MQTT contract against the running backend
  (telemetry appears at `telemetry/{type}/{key}`, device flips
  online/offline on the WebSocket `device_status` event).
- **Flash-wear bench**: instrument `settings_save_switch_rule` call count over
  1 hour to confirm the coalescing fix drops writes to ~0 steady-state.
- **Safety bench**: force a reboot with a relay energized + auto-trip on →
  confirm safe-state-on-boot holds the relay OFF until rules evaluate.
- **BLE brute-force bench**: confirm persistent failed-PIN backoff/lockout
  across reconnects.
- **Regression**: keep `~/.platformio/venv/bin/pio run` green across all four
  envs (`esp32dev`, `esp32c3`, `esp32c3_nodisplay`, `esp32s3`).

## 8. Sequencing & risk notes
- **Do P0 before fielding any device** — items P0-1, P0-2, P0-5, P0-6, P0-7,
  P0-8 are silent data-loss, flash-wear, or interop failures.
- P1-1/P1-2 (relay safety) are safety-critical for any device controlling a
  load; pair them with the safe-state-on-boot interlock.
- The BLE unification (P1-7) and the MQTT contract (P0-1) are the two largest
  refactors; everything else is localized.
- Several "smells" (raw `memcpy` blobs, size-only compat) become real bugs
  the next time a persisted struct gains a field — version+pack them while
  the relevant files are open for the P0/P1 work.