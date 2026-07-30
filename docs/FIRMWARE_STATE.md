# Firmware State — Power Monitor v2

**Version:** `2.0.0` (TELEMETRY_FW_VERSION)  
**Framework:** PlatformIO / Arduino on ESP32  
**Boards:** esp32dev (xtensa), esp32c3 (RISC-V), esp32s3  
**Last updated:** 2026-07-30

---

## 1. Task Architecture (3 FreeRTOS Tasks + Arduino loop)

| Task | Core | Stack | Priority | Period | Role |
|---|---|---|---|---|---|
| **Sensor** | 1 (or 0 on unicore) | 4 KB | 4 (highest) | 1 s | I2C reads, logging, coulomb/energy/cycle integration, switch eval |
| **Network** | 0 | 16 KB | 3 | 10 ms yield | WiFi/MQTT/Supabase/HTTP, BLE, display update, settings polling |
| **UI** | 1 (or 0 on unicore) | 2 KB | 2 | 50 ms | Button debounce, LED patterns, display page cycling |
| **loop()** | Arduino | — | 1 (lowest) | 10 ms | Serial CLI, heap check |

**Watchdog:** TWDT with 30 s timeout, subscribed by Sensor, Network, and UI tasks. Arduino `loop()` is deliberately NOT subscribed.

---

## 2. Module Inventory (28 source files)

### Core Infrastructure

| Module | Files | Purpose |
|---|---|---|
| **main** | `src/main.cpp` | `setup()` + `loop()` — creates tasks, serial CLI (50+ commands) |
| **config** | `include/config.h` | Compile-time constants, board pinouts, I2C addresses, feature flags |
| **settings_manager** | `src/settings_manager.cpp` | NVS persistence (Preferences) — WiFi, MQTT, Supabase, relay rules, calibration, battery profiles, BLE PIN, virtual channels, discovered sensors |
| **core_shared** | `src/core_shared.cpp` | Inter-task queues: `g_sensor_queue` (SensorSnapshot), `g_cmd_queue` (JSON commands), `g_relay_mutex` |
| **device_identity** | `src/device_identity.cpp` | MAC-derived serial number ("PM-AABBCCDDEEFF"), HW revision, crash counter, clean-shutdown marker |
| **device_state** | `src/device_state.cpp` | `build_device_state()` — point-in-time health snapshot (WiFi, BLE, storage, sensors, services) |
| **event_log** | `src/event_log.cpp` | 64-entry ring buffer of device events (debug/info/warn/error) with subsystem tags |
| **error_reporter** | `src/error_reporter.cpp` | 8-entry ring buffer of subsystem errors, drained by telemetry publish |
| **log_serial** | `include/log_serial.h` | `LOG_PRINT`/`LOG_PRINTLN` macros — no-ops when `HAS_SERIAL=0` |

### Sensors

| Module | Files | Purpose |
|---|---|---|
| **sensor_manager** | `src/sensor_manager.cpp` | I2C bus init, `read_sensors()`, `discover_sensors()`, baseline calibration, flat-channel helpers |
| **sensor_pod** | `include/sensor_pod.h` | Pod/channel data model — `SensorSnapshot`, `PodState`, `PhysicalChannel` |
| **bl0939_pod** | `src/bl0939_pod.cpp` | BL0939 UART energy-meter frame parser (24-byte frames, CRC) |
| **serial1_manager** | `src/serial1_manager.cpp` | Legacy UART reader for BL0939 (disabled by default) |

### Data Processing

| Module | Files | Purpose |
|---|---|---|
| **data_logger** | `src/data_logger.cpp` | 16 KB RAM ring buffer, delta-compressed (BaseEntry + DeltaEntry), LittleFS overflow fallback |
| **coulomb_counter** | `src/coulomb_counter.cpp` | Per-channel mAh integration (I × dt), NVS-persisted every 5 min |
| **energy_counter** | `src/energy_counter.cpp` | Per-channel Wh integration (P × dt), NVS-persisted |
| **cycle_counter** | `src/cycle_counter.cpp` | DoD-weighted cycle counting, SoC tracking, continuous SoH from completed discharge legs |
| **battery_profile** | `src/battery_profile.cpp` | 16 chemistry profile slots (4 built-in + 12 custom), NVS blob |
| **battery_state** | `src/battery_state.cpp` | Per-channel `BatteryState` (Ah in/out, cycles, SoC, SoH), NVS persistence |
| **battery_nvs** | `src/battery_nvs.cpp` | Low-level NVS blob read/write for battery profiles |

### Control

| Module | Files | Purpose |
|---|---|---|
| **switch_controller** | `src/switch_controller.cpp` | Rule-based switch evaluation (overcurrent, undervoltage, SoC, schedule, channel-compare), pulse mode, auto/manual mode, GPIO validation |
| **battery_lock** | `include/battery_lock.h` | Critical-section lock discipline for BatteryState reads |

### Connectivity

| Module | Files | Purpose |
|---|---|---|
| **connectivity_manager** | `src/connectivity_manager.cpp` | WiFi (NVS→compile-time fallback), MQTT (PubSubClient), Supabase REST, custom HTTP endpoint, NTP sync, log batch publish, settings command polling |
| **ble_provisioner** | `src/ble_provisioner.cpp` | BLE GATT server, PIN-protected JSON command interface, 44 commands, sensor data notify, NimBLE stack lifecycle |
| **telemetry** | `src/telemetry.cpp` | `telemetry_build()` — unified snapshot struct for all transports |
| **telemetry_pb** | `src/telemetry_pb.cpp` | Protobuf encoding via nanopb (plumbing only, not consumed by backend yet) |

### UI

| Module | Files | Purpose |
|---|---|---|
| **display_manager** | `src/display_manager.cpp` | SSD1306 OLED, 5-page cycling (status + 4 channel pages) |
| **ui_manager** | `src/ui_manager.cpp` | LED patterns, button debounce, network status indicator, heartbeat |

---

## 3. Data Flow

### 1-Second Sensor Tick (Sensor Task, Core 1)

```
read_sensors() → SensorSnapshot
  → push_sensor_data() → g_sensor_queue (to Network task)
  → log_sample() → RAM ring buffer (delta-compressed)
  → update_coulomb_counter() → NVS every 5 min
  → update_energy_counter() → NVS every 5 min
  → update_cycle_counter() → SoC/SoH/cycle tracking
  → evaluate_switches() → rule-based relay control
```

### 5-Second Publish Tick (Network Task, Core 0)

```
xQueueReceive(g_sensor_queue) → SensorSnapshot
  → telemetry_build() → TelemetrySnapshot
  → publish_data() → MQTT
  → publish_data_supabase() → Supabase REST
  → update_display() → OLED (every 5 s)
  → publish_log_batch_supabase() → drain log buffer
  → check_settings_commands() → poll Supabase for pending config
  → try_sync_epoch_time() → NTP retry (every 60 s)
```

### Continuous Loops

```
loop_connectivity() — WiFi state machine, MQTT reconnect, BLE notify
loop_ble_provisioner() — BLE GATT event processing
loop_ui() — button debounce, LED patterns
handle_serial_cli() — 50+ serial commands
```

---

## 4. Key Data Structures

### SensorSnapshot (inter-task queue)

```cpp
struct SensorSnapshot {
    uint32_t timestamp_ms;
    uint8_t num_pods;
    PodState pods[MAX_PODS];  // MAX_PODS = 8
    uint8_t total_logical_channels;
};

struct PodState {
    uint8_t id;
    PodType type;  // POD_INA226 or POD_BL0939
    char name[16];
    uint8_t num_channels;
    PhysicalChannel channels[MAX_CHANNELS_PER_POD];  // 2
};

struct PhysicalChannel {
    uint8_t pod_id;
    uint8_t pod_channel;
    float voltage;
    float current;
    float power;
    float energy_Wh;
    float coulomb_mAh;
    SampleMeta meta;  // stddev, spike
};
```

### TelemetrySnapshot (publish payload)

```cpp
struct TelemetrySnapshot {
    uint32_t ts;                    // unix epoch seconds
    uint16_t ts_ms;                 // sub-second resolution
    uint8_t  schema_version;        // = 1
    char     schema[16];            // "telemetry_v1"
    TelemetryDevice device;         // id, fw, uptime_ms
    TelemetryWifi   wifi;           // rssi, ip
    uint8_t channel_count;          // ≤ 16
    uint8_t switch_count;           // ≤ 8
    uint8_t battery_count;          // ≤ 8
    TelemetryChannel channels[16];  // V, I, P, energy_Wh, charge_mAh
    TelemetrySwitch  switches[8];   // type, state, auto_mode, rule_tripped
    TelemetryBattery battery[8];    // profile_id, chemistry, soc_pct, Ah_in/out, cycles, soh_pct
    TelemetryLogMeta log;           // entries, overflow
    uint32_t heap_free;
};
```

### DeviceState (status snapshot)

```cpp
struct DeviceState {
    // System: uptime_ms, free_heap, min_free_heap, reset_reason, serial, hw_rev, crash_count, safe_mode
    // WiFi: connected, rssi, ip, ntp_synced
    // BLE: active, connected
    // Services: mqtt_connected, http_configured, supabase_configured, network_skipped
    // Storage: sd_present, log_entries, log_buffer_used_pct, log_overflow
    // Sensors: channel_count, switch_count, sensors_calibrating
};
```

---

## 5. Switch/Rule System

**Switch types:** RELAY, MOSFET_LOW_SIDE, MOSFET_HIGH_SIDE, SSR, EXPANDER

**Condition kinds:**
- `SCK_OVERCURRENT` — current exceeds threshold
- `SCK_UNDERVOLTAGE` — voltage below threshold
- `SCK_SOC_LOW` — battery SoC below threshold
- `SCK_SOC_HIGH` — battery SoC above threshold
- `SCK_CHANNEL_ABOVE` — compare to another channel's current
- `SCK_CHANNEL_BELOW` — compare to another channel's current
- `SCK_SCHEDULE_WINDOW` — 7×24h bitmask schedule

**Logic:** AND / OR with `min_conditions` threshold  
**Features:** trip/reset delays (ms), hysteresis (dead-band), pulse mode (non-blocking), auto/manual mode, GPIO denylist (strapping pins rejected)

**Threading model:** `evaluate_switches()` runs on Sensor task (Core 1) at 1 Hz. `switch_set()`, `switch_pulse()`, `switch_set_auto()` may be called from any context (BLE, serial, loop). Manual and auto paths can collide — callers must `switch auto off` for durable manual control.

---

## 6. Battery System

- **16 profile slots** (4 built-in + 12 custom)
- **Chemistry types:** LEAD_ACID, LIION, LFP, LIPO, NICD, NIMH, CUSTOM
- **Per-channel tracking:**
  - Cumulative Ah in/out (coulomb counter)
  - DoD-weighted equivalent full cycles
  - SoC (coulomb-counting from profile capacity + initial_soc_pct)
  - Continuous SoH (EWMA from completed discharge legs)
- **Channel→profile binding** via NVS (0xFF = no binding)

---

## 7. Connectivity

| Transport | Status | Details |
|---|---|---|
| **WiFi** | Active | NVS credentials → compile-time fallback, blocking init, async reconnect |
| **MQTT** | Active | PubSubClient, JSON telemetry, log batch binary topic |
| **Supabase** | Active | REST API, telemetry + log batch + settings commands + calibration sync |
| **BLE** | Active | NimBLE GATT, PIN-protected, 44 commands, sensor data notify, stack lifecycle (init/deinit for heap) |
| **HTTP** | Active | Custom endpoint, JSON POST |
| **Blynk** | Legacy | Configured but likely unused |
| **Protobuf** | Plumbing | `USE_PROTOBUF=0` by default, backend doesn't consume `/pb` topic yet |

---

## 8. Safety & Resilience

- **Safe mode:** 5+ crashes → skip network/BLE init, only sensor task runs
- **Crash counter:** NVS-persisted, incremented on unclean shutdown, reset on healthy boot
- **Watchdog:** 30 s TWDT on Sensor + Network + UI tasks
- **BLE brute-force protection:** NVS-persisted fail counter
- **GPIO denylist:** Per-board strapping pins, USB, flash pins rejected
- **Delta sanity clamp:** dt clamped to [0, 10] s to prevent integration drift
- **Factory reset:** Wipes all NVS, reboots
- **Telemetry overflow queue:** SD card fallback when MQTT publish fails
- **Log overflow:** LittleFS file when RAM buffer is full and WiFi is disconnected

---

## 9. Serial CLI (50+ Commands)

**Status:** `status`, `mem`, `sensors`, `log`, `help`  
**Switches:** `switch N 0/1`, `switch status`, `switch rules N`, `switch auto on/off`, `test switch N`, `test all switches`  
**Battery:** `battery list`, `battery bind N id`, `battery show N`, `battery reset N`, `battery profile show/set/reset/delete`  
**Calibration:** `cal N type value`, `cal show`, `calibrate_baseline`, `shunt N ohms`, `vratio N ratio`, `resistor N r_high r_low`  
**Network:** `wifi_ssid`, `wifi_pass`, `set_wifi`, `wifi_show`, `supabase ...`, `supabase_show`, `ble_on`  
**Diagnostics:** `i2c_scan`, `discover_sensors`, `test sensor N`, `test all sensors`, `test display`, `serial1peek`  
**Control:** `flush log`, `factory_reset`, `reboot`, `virtual_channel N ...`, `display page N`, `display all`

---

## 10. Build Environments

| Env | Board | Display | Notes |
|---|---|---|---|
| `esp32dev` | ESP32 Dev | SSD1306 | Default target, dual-core |
| `esp32c3` | ESP32-C3 | SSD1306 | RISC-V, single-core |
| `esp32c3_nodisplay` | ESP32-C3 | None | Saves flash, USB-CDC console |

---

## 11. Known Residual Issues

- **Non-blocking paths:** WiFi init is still blocking (hangs if AP unreachable)
- **BLE off-host:** NimBLE stack deinit on WiFi reconnect frees ~50 KB heap but BLE re-init can fail if heap is fragmented
- **OTA:** Not yet implemented
- **Log timestamps:** Before NTP sync, timestamps are uptime-relative (marked untrusted)
- **Protobuf:** Firmware-side encoding exists but backend doesn't consume the `/pb` topic — no content negotiation
- **Blynk:** Legacy integration still compiled in but likely unused
- **INA3221:** Legacy 3-channel module support kept for migration only, disabled by default
- **Serial1/BL0939:** UART-based energy meter support disabled by default
