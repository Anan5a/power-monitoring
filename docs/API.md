# Power Monitor v2 — Interface & API Documentation

This document describes all external and internal interfaces for the ESP32 power monitoring firmware.

---

## Table of Contents

1. [Hardware Pinout](#hardware-pinout)
2. [Module Overview](#module-overview)
3. [Serial CLI](#serial-cli)
4. [BLE GATT Interface](#ble-gatt-interface)
5. [MQTT Topics & Payloads](#mqtt-topics--payloads)
6. [HTTP Endpoint](#http-endpoint)
7. [Data Structures](#data-structures)
8. [Settings Persistence (NVS)](#settings-persistence-nvs)
9. [Data Logging Format](#data-logging-format)
10. [Build Instructions](#build-instructions)

---

## Hardware Pinout

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
| `wifi_show` | Show current WiFi SSID |
| `wifi_ssid <ssid>` | Set WiFi SSID |
| `wifi_pass <password>` | Set WiFi password |
| `set_wifi <ssid> <password>` | Set both WiFi SSID and password at once |
| `supabase_show` | Show Supabase URL, anon key, device key |
| `supabase <url> <anon_key> <service_role_key> <device_key>` | Configure Supabase connection |
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
```json
{"cmd":"get_mqtt","pin":123456}
```
Response: `{"ok":true,"broker":"192.168.1.100","port":1883,"topic":"power-monitor/data"}`

#### `get_http` — Get stored HTTP endpoint (token masked)
```json
{"cmd":"get_http","pin":123456}
```
Response: `{"ok":true,"url":"https://api.example.com/v1/data","token":"***","enabled":true}`

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

---

## MQTT Topics & Payloads

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
