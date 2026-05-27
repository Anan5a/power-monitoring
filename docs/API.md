# Power Monitor v2 — Interface & API Documentation

This document describes all external and internal interfaces for the ESP32 power monitoring firmware.

---

## Table of Contents

1. [Quick Start Setup](#quick-start-setup)
2. [Hardware Pinout](#hardware-pinout)
3. [Module Overview](#module-overview)
4. [I2C Sensor Architecture](#i2c-sensor-architecture)
5. [Serial CLI](#serial-cli)
6. [BLE GATT Interface](#ble-gatt-interface)
7. [Supabase Telemetry](#supabase-telemetry)
8. [Dashboard](#dashboard)
9. [Calibration Guide](#calibration-guide)
10. [Relay Logic](#relay-logic)
11. [Settings Persistence (NVS)](#settings-persistence-nvs)
12. [Data Logging Format](#data-logging-format)
13. [Build Instructions](#build-instructions)

---

## Quick Start Setup

### Prerequisites

- ESP32 dev board (ESP32-WROOM-32 or ESP32-C3)
- INA3221 3-channel voltage/current monitor (0x40 + 0x42)
- Optional: INA226 high-side monitor (0x41), ADS1115 ADC (0x48)
- Supabase project (free tier works)

### Steps

**1. Build and flash firmware**

```bash
git clone https://github.com/Anan5a/power-monitoring.git
cd power-monitoring

# Set your WiFi credentials in include/config.h before flashing
# Edit WIFI_SSID, WIFI_PASSWORD

# Build and flash
~/.platformio/venv/bin/pio run -e esp32dev --target upload
~/.platformio/venv/bin/pio device monitor
```

**2. Set up Supabase**

Create a project at supabase.com, then run the schema:

```sql
-- In Supabase SQL editor (Project > SQL Editor)
-- Apply: backend/supabase/schema.sql
```

Required tables: `devices`, `telemetry_live`, `device_channels`, `sensor_calibration_status`, `relay_states`, `settings_commands`, `profiles`.

**3. Provision device via BLE**

With the ESP32 running and BLE enabled:
1. Open the dashboard UI
2. Go to **Provisioning** page
3. Enter your Supabase URL, anon key, service role key
4. Enter device name and BLE PIN (default `123456`)
5. Configure WiFi credentials
6. Save — device will reboot and connect

**4. Configure via dashboard**

- **Channels tab** → name each virtual channel, assign voltage/current sources
- **Settings tab** → battery capacity per channel, relay thresholds
- **Sensor Controls** → run baseline calibration, set manual offsets

---

## Hardware Pinout

| Function | GPIO | Notes |
|---|---|---|
| I2C SDA | 16/21 | Configurable in `config.h` |
| I2C SCL | 17/22 | Configurable in `config.h` |
| Relay 1 | 25 | Default, configurable |
| Relay 2 | 26 | Default, configurable |
| Relay 3 | 27 | Default, configurable |
| Relay 4 | 14 | Default, configurable |

**I2C Addresses (from `config.h`):**

| Device | Address | Purpose |
|---|---|---|
| INA3221 (current) | `0x40` | 3-channel current measurement |
| INA3221 (voltage) | `0x42` | 3-channel voltage measurement |
| INA226 | `0x41` | High-side current/power (optional) |
| ADS1115 | `0x48` | 4-channel 16-bit ADC (optional) |
| SSD1306 OLED | `0x3C` | 128×64 display (optional) |

**Sensor sources per virtual channel:**

Each of the 4 virtual channels (VC0–VC3) can be mapped to any combination of:
- `voltage_src`: 1=INA3221Voltage(0x42), 2=INA3221Current(0x40), 3=INA226, 4=ADS1115
- `current_src`: 2=INA3221Current(0x40), 3=INA226

Power = `V × I` for normal sources; INA226 computes power internally.

| Function | GPIO | Notes |
|---|---|---|
| I2C SDA | 21 | Shared by all I2C devices |
| I2C SCL | 22 | Shared by all I2C devices |
| Relay 1 | 25 | Default, configurable via BLE |
| Relay 2 | 26 | Default, configurable via BLE |
| Relay 3 | 27 | Default, configurable via BLE |
| Relay 4 | 14 | Default, configurable via BLE |

**I2C Addresses:**
- INA3221: `0x40` (3-channel voltage/current)
- INA226: `0x41` (1-channel voltage/current/power)
- ADS1115: `0x48` (4-channel ADC)
- OLED: `0x3C` (SSD1306 128x64)

---

## Module Overview

| File | Responsibility |
|---|---|
| `src/main.cpp` | Dual-timer loop (1s sensors/logging/relays, 5s publish/display), Serial CLI |
| `src/sensor_manager.cpp` | I2C init, reads INA3221/INA226/ADS1115 |
| `src/connectivity_manager.cpp` | WiFi, MQTT, Blynk, HTTP, BLE notify |
| `src/ble_provisioner.cpp` | BLE GATT server, JSON command interface, PIN security |
| `src/display_manager.cpp` | SSD1306 OLED, 5-page cycling (status + ch0-3) |
| `src/data_logger.cpp` | 32KB RAM delta-compressed circular buffer, SPIFFS fallback |
| `src/coulomb_counter.cpp` | Per-channel mAh accumulator, 5-min NVS persist |
| `src/relay_controller.cpp` | Relay logic with trip/reset delays, SoC support |
| `src/settings_manager.cpp` | NVS key-value persistence via `Preferences` |

---

## Serial CLI

Connect at `115200 baud`. Type commands and press Enter.

| Command | Description |
|---|---|
| `status` | Print IP, log entries, overflow status, coulomb mAh per channel, SoC if battery configured |
| `sensors` | Print raw sensor readings (all channels) |
| `relay status` | Print all relay configurations and current GPIO states |
| `relay N 0` / `relay N 1` | Manually set relay `N` OFF / ON (bypasses logic) |
| `reset coulomb N` | Reset coulomb counter for channel `N` (0-3) |
| `flush log` | Pop and print size of all buffered log entries |
| `i2c_scan` | Scan I2C bus and list responding addresses |
| `factory_reset` | Wipe all NVS settings and reboot |
| `test relay N` | Pulse relay N for 3s then deactivate |
| `test all relays` | Sequence-test all 4 relays |
| `test sensor N` | Read single sensor channel (0-2) |
| `test all sensors` | Read all sensor channels |
| `test display` | Cycle OLED pages 5 times |
| `display page N` | Print display page N (0=status, 1-3=ch0-2, 4=INA226) |
| `display all` | Print all display pages |
| `relay auto on` / `relay auto off` | Enable/disable auto-trip relay logic |
| `shunt N ohms` | Set INA3221 shunt resistance for channel N (0=clear) |
| `shunt show` | Show current shunt settings for all channels |
| `vratio N ratio` | Set voltage divider ratio for channel N (0=clear) |
| `vratio show` | Show current voltage ratios for all channels |
| `resistor N r_high r_low` | Set resistor values, ratio auto-computed as (r_high+r_low)/r_low |
| `resistor show` | Show resistor values and computed ratios per channel |
| `cal N type value` | Set calibration for channel N. type: 0=volt_offset_mv, 1=volt_gain, 2=curr_offset_ma, 3=curr_gain |
| `cal show` | Show calibration values for all channels |
| `calibrate_baseline` | Restart baseline noise calibration — collects new baseline over next 10 ticks (~5 seconds). Spike detection resumes after completion. |
| `wifi_show` | Show current WiFi SSID |
| `wifi_ssid <ssid>` | Set WiFi SSID |
| `wifi_pass <password>` | Set WiFi password |
| `set_wifi <ssid> <password>` | Set both WiFi SSID and password at once |
| `supabase_show` | Show Supabase URL, anon key, device key |
| `supabase <url> <anon_key> <service_role_key> <device_key>` | Configure Supabase connection |
| `virtual_channel show` | Show all 4 virtual channel configs (src:idx for V and I) |
| `virtual_channel N` | Show virtual channel config for channel N (0-3) |
| `virtual_channel N vs vidx cs cidx` | Set CH N: V=src vs:idx, I=src cs:idx (src: 0=none 1=volt 2=curr 3=ina226 4=ads1115) |
| `serial1peek` | Dump up to 5 lines from Serial1 RX buffer |
| `reboot` | Reboot the device |
| `help` | Show command list |

---

## Voltage & Current Calibration

Two-level calibration system applied in order:

1. **Ratio** (channel-specific) — converts raw mV to volts
   - `resistor N r_high r_low` — ratio = (r_high + r_low) / r_low
   - `vratio N ratio` — direct ratio override
   - Falls back to `VOLT_RATIO_CHn` from config.h
2. **Fine adjustment** — offset and gain applied to mV before/after ratio
   - `cal N 0 value` — `volt_offset_mv` in mV (zero-point shift)
   - `cal N 1 value` — `volt_gain` multiplier (e.g. 1.02 = +2% correction)
   - `cal N 2 value` — `curr_offset_ma` in mA (subtract ghost current)
   - `cal N 3 value` — `curr_gain` multiplier

**Math (voltage):** `displayed_V = (raw_mV + volt_offset_mv) * volt_gain * ratio / 1000`

**Math (current):** `displayed_A = (raw_A * 1000 - curr_offset_ma) * curr_gain / 1000`

**Example:** If CH2 reads 11.27V but should be 11.20V:
```
cal 2 1 0.9938   # gain = expected/actual = 11.20/11.27
```

---

## BLE GATT Interface

The firmware exposes a single BLE service with 4 characteristics.

**Device Name:** `PowerMonitor`
**Service UUID:** `4fafc201-1fb5-459e-8fcc-c5c9c331914b`

| Characteristic | UUID | Properties | Purpose |
|---|---|---|---|
| `cmd` | `c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4` | WRITE | Receive JSON commands from browser/phone |
| `resp` | `d8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01` | READ, NOTIFY | JSON responses to commands |
| `status` | `e3c5a7f2-8b1d-4e6c-9a0f-2d4b6e8c1a35` | READ, NOTIFY | Periodic status broadcast (every 10s) |
| `sensor` | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | READ, NOTIFY | Live sensor JSON (mirrors MQTT payload) |

### Security Model

- If no PIN is set (`settings_load_ble_pin() == 0`), all commands work without a PIN.
- If a PIN is set, commands requiring authentication must include `"pin": <6-digit>`.
- `set_pin` command: requires `old_pin` if a PIN already exists.

### BLE Commands

All commands are JSON strings written to the `cmd` characteristic.

#### `set_wifi` — Save WiFi credentials
```json
{"cmd":"set_wifi","ssid":"MyNetwork","pass":"MyPassword","pin":123456}
```
Response: `{"ok":true,"msg":"wifi_saved_reboot"}`

#### `set_mqtt` — Save MQTT broker settings
```json
{"cmd":"set_mqtt","broker":"192.168.1.100","port":1883,"topic":"power-monitor/data","pin":123456}
```
Response: `{"ok":true,"msg":"mqtt_saved"}`

#### `set_http` — Save custom HTTP endpoint
```json
{"cmd":"set_http","url":"https://api.example.com/v1/data","token":"Bearer abc123","enabled":true,"pin":123456}
```
Response: `{"ok":true,"msg":"http_saved"}`

#### `set_relay` — Configure a relay rule
```json
{"cmd":"set_relay","idx":0,"channel":0,"overcurrent_A":5.0,"undervoltage_V":10.5,"soc_low_pct":20.0,"soc_high_pct":95.0,"trip_delay_ms":1000,"reset_delay_ms":5000,"gpio_pin":25,"active_high":true,"enabled":true,"pin":123456}
```
Response: `{"ok":true,"msg":"relay_saved"}`

#### `set_battery` — Configure battery for SoC calculation
```json
{"cmd":"set_battery","channel":0,"capacity_mAh":5000.0,"initial_soc_pct":100.0,"pin":123456}
```
Response: `{"ok":true,"msg":"battery_saved"}`

#### `set_pin` — Set or change BLE PIN
```json
{"cmd":"set_pin","old_pin":0,"new_pin":123456}
```
Response: `{"ok":true,"msg":"pin_updated"}`

#### `reset_coulomb` — Reset coulomb counter
```json
{"cmd":"reset_coulomb","channel":0,"pin":123456}
```
Response: `{"ok":true,"msg":"coulomb_reset"}`

#### `get_status` — Get runtime status
```json
{"cmd":"get_status","pin":123456}
```
Response:
```json
{"ok":true,"entries":12345,"overflow":false,"relay_count":4}
```

#### `get_relay` — Get relay configuration
```json
{"cmd":"get_relay","idx":0,"pin":123456}
```
Response:
```json
{"ok":true,"idx":0,"channel":0,"overcurrent_A":5.0,"undervoltage_V":10.5,"soc_low_pct":20.0,"soc_high_pct":95.0,"trip_delay_ms":1000,"reset_delay_ms":5000,"gpio_pin":25,"active_high":true,"enabled":true}
```

#### `get_battery` — Get battery config
```json
{"cmd":"get_battery","channel":0,"pin":123456}
```
Response:
```json
{"ok":true,"channel":0,"capacity_mAh":5000.0,"initial_soc_pct":100.0}
```

#### `get_wifi` — Get stored WiFi (password masked)
```json
{"cmd":"get_wifi","pin":123456}
```
Response: `{"ok":true,"ssid":"MyNetwork","pass":"***"}`

#### `get_mqtt` — Get stored MQTT settings
```json
{"cmd":"get_mqtt","pin":123456}
```
Response: `{"ok":true,"broker":"192.168.1.100","port":1883,"topic":"power-monitor/data"}`

#### `set_calibration` — Set channel calibration (offset/gain)
```json
{"cmd":"set_calibration","channel":0,"type":0,"value":0.0,"pin":123456}
```
- `type=0`: `volt_offset_mv` (mV zero shift)
- `type=1`: `volt_gain` (multiplier, e.g. 1.02)
- `type=2`: `curr_offset_ma` (mA ghost current)
- `type=3`: `curr_gain` (multiplier)
Response: `{"ok":true,"msg":"calibration_saved"}`

#### `get_calibration` — Get channel calibration
```json
{"cmd":"get_calibration","channel":0,"pin":123456}
```
Response:
```json
{"ok":true,"channel":0,"volt_offset_mv":0.0,"volt_gain":1.0,"curr_offset_ma":12.0,"curr_gain":1.0}
```

#### `reset_calibration` — Reset channel calibration to defaults
```json
{"cmd":"reset_calibration","channel":0,"pin":123456}
```
Response: `{"ok":true,"msg":"calibration_reset"}`

#### `set_virtual_channel` — Configure a virtual channel (source mapping)
```json
{"cmd":"set_virtual_channel","channel":0,"voltage_src":1,"voltage_idx":0,"current_src":2,"current_idx":0,"pin":123456}
```
- `channel`: 0-3 (virtual channel index)
- `voltage_src`: 0=none, 1=ina3221_volt(0x42), 2=ina3221_curr(0x40), 3=ina226, 4=ads1115
- `voltage_idx`: channel index within that source (0-2 for dual INA3221, 0 for INA226, 0-3 for ADS1115)
- `current_src`: 0=none, 1=ina3221_curr(0x40), 2=ina226
- `current_idx`: channel index (0-2 for INA3221, 0 for INA226)
- When both voltage and current are set, power is computed as V×I (or from INA226 built-in power if using src=3 for current)
- Virtual channels appear in MQTT/Supabase payloads as `ch0_V`, `ch0_I`, `ch0_P` ... `ch3_P`
Response: `{"ok":true,"msg":"virtual_channel_saved"}`

#### `get_virtual_channel` — Get virtual channel configuration
```json
{"cmd":"get_virtual_channel","channel":0,"pin":123456}
```
Response: `{"ok":true,"channel":0,"voltage_src":1,"voltage_idx":0,"current_src":2,"current_idx":0}` or `{"ok":false,"error":"virtual_channel_not_found"}`

#### `set_shunt` — Set INA3221 shunt resistance
```json
{"cmd":"set_shunt","channel":0,"ohms":0.0003,"pin":123456}
```
- Set to `0` to clear (uses library default)
- Stored in NVS, applied at boot via `ina3221.setShuntResistance()`
Response: `{"ok":true,"msg":"shunt_saved"}`

#### `get_shunt` — Get INA3221 shunt resistance
```json
{"cmd":"get_shunt","channel":0,"pin":123456}
```
Response: `{"ok":true,"channel":0,"ohms":0.0003}`

#### `set_vratio` — Set voltage divider ratio
```json
{"cmd":"set_vratio","channel":0,"ratio":3.521,"pin":123456}
```
- Ratio stored in NVS, overrides config.h default
- Or use `set_resistors` to compute ratio from resistor values
Response: `{"ok":true,"msg":"vratio_saved"}`

#### `set_resistors` — Set voltage divider resistor values (ratio computed)
```json
{"cmd":"set_resistors","channel":0,"r_high":300000,"r_low":119000,"pin":123456}
```
- Ratio computed as `(r_high + r_low) / r_low`
- Stored in NVS, applied at boot
Response: `{"ok":true,"ratio":3.521}`

#### `get_http` — Get stored HTTP endpoint (token masked)
```json
{"cmd":"get_http","pin":123456}
```
Response: `{"ok":true,"url":"https://api.example.com/v1/data","token":"***","enabled":true}`

#### `calibrate_baseline` — Restart baseline noise calibration
```json
{"cmd":"calibrate_baseline","pin":123456}
```
Resets `baseline_stddev[]` and re-collects spike detection baseline over next 10 ticks (~5 seconds). During collection, `sensor_calibration_status` table is updated each tick with `baseline_tick=N` and current stddev values. No spike detection until calibration completes.

Also available via **Supabase command poll** (no PIN required): insert `settings_commands` row with `cmd_type=calibrate_baseline`.
Response: `{"ok":true,"msg":"baseline_calibration_started"}`

#### `factory_reset` — Wipe all NVS settings
```json
{"cmd":"factory_reset","pin":123456}
```
Response: `{"ok":true,"msg":"factory_reset_done_reboot"}`

### Error Responses

| Error | Meaning |
|---|---|
| `{"ok":false,"error":"bad_json"}` | Malformed JSON |
| `{"ok":false,"error":"invalid_pin"}` | Wrong PIN provided |
| `{"ok":false,"error":"invalid_old_pin"}` | Wrong old PIN when changing PIN |
| `{"ok":false,"error":"unknown_cmd"}` | Command string not recognized |
| `{"ok":false,"error":"relay_not_found"}` | Relay index does not exist |
| `{"ok":false,"error":"battery_not_found"}` | Battery config not found for channel |
| `{"ok":false,"error":"wifi_not_set"}` | No WiFi credentials stored |
| `{"ok":false,"error":"mqtt_not_set"}` | No MQTT settings stored |
| `{"ok":false,"error":"http_not_set"}` | No HTTP endpoint stored |
| `{"ok":false,"error":"virtual_channel_not_found"}` | Virtual channel not configured for that index |

---

## Supabase Telemetry

The ESP32 publishes telemetry to Supabase every ~5 seconds via the `insert_telemetry` RPC function. The dashboard uses Supabase Realtime to display live data without polling.

### Supabase Tables

| Table | Purpose | ESP32 writes | Dashboard reads |
|---|---|---|---|
| `telemetry_live` | Latest readings per device | Every ~5s | Realtime subscription |
| `sensor_calibration_status` | Baseline calibration progress | Every 500ms during calibration | Polled every 1s |
| `relay_states` | Relay on/off states | Loop reads this to check relay toggles | Writes on toggle |
| `settings_commands` | Pending config commands | Polls every 10s | Inserts commands |
| `device_channels` | Channel names, battery profiles, calibration | ESP reads + writes | Modifies via Settings tab |

### Telemetry Payload

Published via `POST /rest/v1/rpc/insert_telemetry` with auth headers.

```json
{
  "p_device_key": "my-device",
  "p_device_api_key": "uuid-here",
  "p_payload": {
    "ina3221_v0": 12.34,
    "ina3221_v1": 12.30,
    "ina3221_v2": 5.10,
    "ina3221_i0": 0.523,
    "ina3221_i1": 0.481,
    "ina3221_i2": 0.0,
    "ina3221_i0_stddev": 0.012,
    "ina3221_i0_spike": false,
    "ina3221_v0_stddev": 0.008,
    "ina3221_v0_spike": false,
    "ina226_v": 12.36,
    "ina226_i": 0.510,
    "ina226_p": 6.303,
    "coulomb_mah0": 850,
    "coulomb_mah1": 720,
    "soc_pct0": 42.5,
    "soc_pct1": 36.0,
    "ch0_V": 12.34,
    "ch0_I": 0.523,
    "ch0_P": 6.47,
    "ch1_V": 12.30,
    "ch1_I": 0.481,
    "ch1_P": 5.92,
    "log_entries": 1234,
    "log_overflow": false
  },
  "p_metadata": {
    "rssi": -45,
    "vcc": 3.28,
    "uptime_s": 3600
  },
  "p_recorded_at": 1716825000
}
```

### Virtual Channel Keys

Virtual channels compute `V × I` from mapped sensor sources and appear as:
- `ch0_V`, `ch0_I`, `ch0_P` — Virtual Channel 0
- `ch1_V`, `ch1_I`, `ch1_P` — Virtual Channel 1
- `ch2_V`, `ch2_I`, `ch2_P` — Virtual Channel 2
- `ch3_V`, `ch3_I`, `ch3_P` — Virtual Channel 3

### Spike Detection Keys

When a spike is detected on INA3221 channels, additional fields appear:
- `ina3221_i0_spike: true` — current spike on channel 0
- `ina3221_v0_spike: true` — voltage spike on channel 0
- `ina3221_i0_stddev: 0.012` — sample stddev of the last burst (mA)
- `ina3221_v0_stddev: 0.008` — sample stddev of the last burst (mV)

### Adaptive Retention

A cron job runs `archive_and_purge_telemetry()` every 10 minutes. When `telemetry_live` exceeds 70% of 500MB (~350MB), oldest rows are deleted until the table reaches 65%. Devices with no telemetry in 24 hours are marked offline.

---

## Dashboard

The React dashboard (in `ui/`) connects to both Supabase and the ESP32 via BLE.

### Pages

| Route | Description |
|---|---|
| `/dashboard` | Main view: VC cards, quick stats, power history chart, sensor controls, relay toggles |
| `/channels` | Tabbed view: raw sensors (INA3221/INA226/ADS1115), virtual channel info, battery SoC per channel, relay config |
| `/settings` | Configure channel names, virtual channel source mapping, battery profiles, voltage ratios, shunt resistors, BLE provisioning |
| `/admin` | Device management: add/remove devices |
| `/provisioning` | BLE device setup: WiFi, Supabase credentials, device key registration |

### Dashboard Realtime Flow

1. ESP32 publishes telemetry to `telemetry_live` every ~5s
2. Supabase Realtime fires INSERT event
3. `useRealtime()` hook in UI receives event and updates `latestReading`
4. All dashboard components re-render with new values
5. PowerHistoryChart accumulates up to 200 data points per range

### Sensor Calibration Panel

Below QuickStatsRow, the **Sensor Controls** panel provides:
- **Run Baseline Calibration** — sends `calibrate_baseline` via `settings_commands`, polls `sensor_calibration_status` every 1s for progress (N/10), shows progress bar, "Done ✓" for 3s after completion
- **Per-channel offsets** — V offset (mV, type 0) and I offset (mA, type 2) inputs with Set buttons for channels 0–2

---

## Calibration Guide

### Baseline Noise Calibration

Run when sensor wiring or environment changes — this recalculates the "quiet" stddev threshold for spike detection.

```
calibrate_baseline
```

Or via BLE: `{"cmd":"calibrate_baseline","pin":123456}`

With nothing connected (or known-load only), run the command. After 10 ticks (~5 seconds):
- Spike detection becomes active with new noise baseline
- `sensor_calibration_status` shows `baseline_tick=10, calibrating=false`

### Manual Zero Calibration

If a channel reads a non-zero value with nothing connected, set the offset:

1. Read the noisy zero with `sensors` command — note the current reading
2. Set negative offset to cancel it:
   ```
   cal 0 2 -12.5   # CH0 current reads +12.5mA with nothing connected → set -12.5mA offset
   ```
3. Verify with `cal show` and re-read `sensors`
4. The channel now reads ~0.000 when open

### Voltage Gain Calibration

With a known accurate voltage source (multimeter):
1. Measure actual voltage at the sensor input
2. Compare to what ESP32 reports
3. Apply gain correction:
   ```
   cal 0 1 1.023   # reads 11.77V but actual is 12.05V → gain = 12.05/11.77 = 1.0238
   ```

### When to Run Baseline Calibration

- After any wiring change to sensors
- After moving the device to a different electrical environment
- If spike detection becomes too sensitive or not sensitive enough
- After factory reset (runs automatically on first 10 ticks anyway)

---

## Relay Logic

| Topic | Direction | Payload |
|---|---|---|
| `power-monitor/data` | Publish | JSON sensor snapshot |
| `power-monitor/logbin` | Publish | Base64-encoded delta-compressed log batch |

### Sensor Data Payload (`power-monitor/data`)

```json
{
  "ina3221": [
    {"v": 12.34, "i": 1.234},
    {"v": 12.34, "i": 1.234},
    {"v": 12.34, "i": 1.234}
  ],
  "ina226": {"v": 12.34, "i": 1.234, "p": 15.23},
  "ads1115": [1.234, 2.345, 3.456, 4.567],
  "log_entries": 12345,
  "log_overflow": false,
  "log_overflow_bytes": 0
}
```

### Log Batch Payload (`power-monitor/logbin`)

Base64 string encoding raw binary `BaseEntry` and `DeltaEntry` structs. See [Data Logging Format](#data-logging-format).

---

## HTTP Endpoint

If enabled (`set_http enabled=true`), the firmware sends an HTTP POST with the same JSON payload as MQTT to the configured URL.

| Header | Value | Condition |
|---|---|---|
| `Content-Type` | `application/json` | Always |
| `Authorization` | `<token from settings>` | Only if token length > 0 |

Method: `POST`
Body: Same JSON as MQTT sensor payload.

HTTP responses `200` and `202` are treated as success. Any other response logs an error to Serial.

---

## Data Structures

### SensorData

```cpp
struct SensorData {
    float ina3221_busV[3];      // Volts, channels 0-2
    float ina3221_current[3];   // Amps, channels 0-2
    float ina226_busV;          // Volts
    float ina226_current;       // Amps
    float ina226_power;         // Watts
    float ads1115_volts[4];     // Volts, channels 0-3
};
```

### RelayRule

```cpp
struct RelayRule {
    uint8_t channel;          // 0-3 (sensor channel to monitor)
    float overcurrent_A;      // Trip if current > this. 0 = disabled.
    float undervoltage_V;     // Trip if voltage < this. 0 = disabled.
    float soc_low_pct;        // Trip if SoC < this. 0 = disabled.
    float soc_high_pct;       // Trip if SoC > this. 0 = disabled.
    uint16_t trip_delay_ms;   // Duration condition must persist to energize
    uint16_t reset_delay_ms;  // Duration condition must clear to de-energize
    uint8_t gpio_pin;         // GPIO to drive
    bool active_high;         // true: HIGH=energized, false: LOW=energized
    bool enabled;             // false: ignore this rule
};
```

### BatteryConfig

```cpp
struct BatteryConfig {
    uint8_t channel;            // 0-3
    float capacity_mAh;         // Total battery capacity
    float initial_soc_pct;      // SoC at last coulomb reset (0-100)
};
```

### Calibration

```cpp
struct Calibration {
    float ina3221_v_offset[3];
    float ina3221_i_gain[3];
    float ina226_v_offset;
    float ina226_i_gain;
};
```

### LogSnapshot

```cpp
struct LogSnapshot {
    uint32_t timestamp_ms;
    float voltage[4];    // mV -> V (divided by 1000)
    float current[4];    // mA -> A (divided by 1000)
    float power[4];        // mW -> W (divided by 1000)
};
```

### VirtualChannelConfig

```cpp
struct VirtualChannelConfig {
    uint8_t voltage_src;   // 0=none, 1=ina3221_volt(0x42), 2=ina3221_curr(0x40), 3=ina226, 4=ads1115
    uint8_t voltage_idx;  // channel index within that source (0-2 for dual INA3221, 0 for INA226, 0-3 for ADS1115)
    uint8_t current_src;  // 0=none, 1=ina3221_curr(0x40), 2=ina226
    uint8_t current_idx;   // channel index (0-2 for INA3221, 0 for INA226)
};
```
Stored in NVS as `vc_<ch>`. Virtual channels appear in telemetry payloads as `ch0_V`, `ch0_I`, `ch0_P` ... `ch3_P`.

---

## Settings Persistence (NVS)

All settings are stored in ESP32 NVS using the `Preferences` library under namespace `pm-settings`.

| Key | Type | Description |
|---|---|---|
| `wifi_ssid` | String | WiFi SSID |
| `wifi_pass` | String | WiFi password |
| `mqtt_broker` | String | MQTT broker IP/hostname |
| `mqtt_port` | UShort | MQTT port (default 1883) |
| `mqtt_topic` | String | MQTT publish topic |
| `http_url` | String | HTTP endpoint URL |
| `http_token` | String | HTTP auth token |
| `http_en` | Bool | HTTP enabled |
| `relay_count` | UChar | Number of configured relays |
| `relay_0` ... `relay_N` | Bytes | Serialized `RelayRule` struct |
| `cal` | Bytes | Serialized `Calibration` struct |
| `coul_0` ... `coul_3` | Float | Accumulated mAh per channel |
| `bat_0` ... `bat_3` | Bytes | Serialized `BatteryConfig` struct |
| `ble_pin` | UInt | 6-digit PIN, 0 = no security |

---

## Data Logging Format

The logger stores data in a 32KB RAM circular buffer using delta compression.

### BaseEntry (29 bytes)

Full snapshot, written when:
- First sample after init
- Delta would overflow int16 range
- Time delta > 60 seconds

| Offset | Type | Value |
|---|---|---|
| 0 | `uint8_t` | `0xB0` |
| 1 | `uint32_t` | `timestamp_ms` |
| 5 | `int16_t[4]` | Voltage in mV (channels 0-3) |
| 13 | `int16_t[4]` | Current in mA (channels 0-3) |
| 21 | `int16_t[4]` | Power in mW (channels 0-3) |

### DeltaEntry (25 bytes)

Delta from previous entry, written when values changed within int16 range.

| Offset | Type | Value |
|---|---|---|
| 0 | `uint8_t` | `0xD0` |
| 1 | `uint16_t` | `dt_ms` since previous entry |
| 3 | `int16_t[4]` | `dV` in mV |
| 11 | `int16_t[4]` | `dI` in mA |
| 19 | `int16_t[4]` | `dP` in mW |

### Overflow Behavior

When the RAM buffer is full and WiFi is disconnected, all buffered data is flushed to SPIFFS file `/log_overflow.bin`. Once the buffer is popped (via `log_pop_batch()`), entries are removed from RAM.

### Log Batch Transmission

`publish_log_batch()` (called from main loop) pops up to 512 bytes from the buffer, base64-encodes them, and publishes to `power-monitor/logbin`.

---

## Build Instructions

PlatformIO CLI is installed at `~/.platformio/venv/bin/pio`.

```bash
# Build default target (esp32dev)
~/.platformio/venv/bin/pio run

# Build specific target
~/.platformio/venv/bin/pio run -e esp32dev
~/.platformio/venv/bin/pio run -e esp32c3
~/.platformio/venv/bin/pio run -e esp32c3_nodisplay

# Upload
~/.platformio/venv/bin/pio run -e esp32dev --target upload

# Monitor serial
~/.platformio/venv/bin/pio device monitor

# Build + upload + monitor
~/.platformio/venv/bin/pio run -e esp32dev --target upload && ~/.platformio/venv/bin/pio device monitor
```

### Environments

| Environment | Board | Display | Flash | RAM |
|---|---|---|---|---|
| `esp32dev` | Generic ESP32 dev board | Yes (SSD1306) | ~93% | ~29% |
| `esp32c3` | ESP32-C3-DevKitM-1 | Yes (SSD1306) | ~87% | ~29% |
| `esp32c3_nodisplay` | ESP32-C3-DevKitM-1 | No | ~87% | ~29% |

**Partition table:** `min_spiffs.csv` (1.9MB app, small SPIFFS for log overflow fallback).

---

## Blynk Virtual Pins

| Pin | Value |
|---|---|
| V0 | INA3221 Ch0 Voltage (V) |
| V1 | INA3221 Ch0 Current (A) |
| V2 | INA226 Voltage (V) |
| V3 | INA226 Current (A) |
| V4 | INA226 Power (W) |
| V5 | ADS1115 Ch0 Voltage (V) |

---

## Relay Logic

For each enabled relay rule:

1. **Condition evaluation** (any of the following, OR logic):
   - `current > overcurrent_A` (if > 0)
   - `voltage < undervoltage_V` (if > 0)
   - `SoC < soc_low_pct` (if battery configured and > 0)
   - `SoC > soc_high_pct` (if battery configured and > 0)

2. **State machine:**
   - If condition_met and not yet active → start timer
   - If condition_met for `trip_delay_ms` → energize relay
   - If condition clears → start reset timer
   - If clear for `reset_delay_ms` → de-energize relay

3. **SoC calculation:**
   ```
   SoC% = initial_soc_pct + (coulomb_mAh / capacity_mAh) * 100
   ```
   Clamped to [0, 100]. If no battery config exists for the channel, SoC is ignored.

---

## Coulomb Counter

```
accumulated_mAh += current_A * dt_seconds / 3600.0
```

- Loaded from NVS on boot
- Persisted to NVS every 5 minutes
- Can be reset per-channel via Serial CLI or BLE command
