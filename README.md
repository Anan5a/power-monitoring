# Power Monitor v2

ESP32-based multi-channel power monitoring IoT device with real-time logging, relay control, and BLE provisioning.

## Features

- **4-channel power monitoring**
  - INA3221 (3 channels): voltage + current
  - INA226 (1 channel): voltage + current + power
  - ADS1115 (4 channels): general-purpose ADC

- **Delta-compressed data logging**
  - 32KB RAM circular buffer with automatic delta compression
  - ~20+ hours of 1-second data retention
  - SPIFFS flash fallback when buffer full and network down
  - Batch transmission via MQTT base64 topic

- **Coulomb counting**
  - Per-channel mAh accumulator
  - Battery SoC calculation (configurable capacity)
  - Survives reboot via 5-minute NVS persistence

- **Advanced relay control**
  - 4 GPIO outputs with configurable pins
  - Trip/reset delay hysteresis
  - Triggers: overcurrent, undervoltage, SoC low, SoC high
  - All rules stored in NVS (no re-flash needed)

- **Connectivity**
  - WiFi with NVS-persisted credentials
  - MQTT (PubSubClient) with JSON payloads
  - Blynk IoT virtual pins
  - Custom HTTP endpoint (optional, configurable)

- **Secure BLE provisioning**
  - Web Bluetooth compatible GATT interface
  - PIN-protected JSON commands
  - Provision WiFi, MQTT, HTTP, relays, battery config without re-flashing

- **OLED display**
  - 5-page cycling: status, Ch0, Ch1, Ch2, Ch3
  - Shows voltage, current, power, SoC%
  - Conditional compilation (can be disabled for headless builds)

## Hardware Requirements

| Component | Purpose | I2C Address |
|---|---|---|
| ESP32 or ESP32-C3 | Main MCU | — |
| INA3221 | 3-channel voltage/current | `0x40` |
| INA226 | 1-channel voltage/current/power | `0x41` |
| ADS1115 | 4-channel 16-bit ADC | `0x48` |
| SSD1306 0.96" OLED | Status display | `0x3C` |

## Wiring

```
ESP32          I2C Bus
------         -------
GPIO21 (SDA) ----+---- INA3221 SDA
                 |
GPIO22 (SCL) ----+---- INA3221 SCL
                 |
                 +---- INA226 SDA/SCL
                 |
                 +---- ADS1115 SDA/SCL
                 |
                 +---- OLED SDA/SCL

Relay GPIOs (default):
  Relay 1 -> GPIO25
  Relay 2 -> GPIO26
  Relay 3 -> GPIO27
  Relay 4 -> GPIO14
```

## Quick Start

### 1. Install dependencies

```bash
# PlatformIO CLI should be installed
~/.platformio/venv/bin/pio --version
```

### 2. Configure credentials

Edit `include/config.h` with your WiFi and MQTT credentials, or leave defaults and provision via BLE later.

```cpp
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"
#define MQTT_BROKER   "192.168.1.100"
#define BLYNK_AUTH_TOKEN "YOUR_BLYNK_TOKEN"
```

### 3. Build and flash

```bash
# For ESP32 dev board (default)
~/.platformio/venv/bin/pio run -e esp32dev --target upload

# For ESP32-C3 supermini
~/.platformio/venv/bin/pio run -e esp32c3 --target upload

# For ESP32-C3 without display (saves flash)
~/.platformio/venv/bin/pio run -e esp32c3_nodisplay --target upload
```

### 4. Monitor serial output

```bash
~/.platformio/venv/bin/pio device monitor
```

Type `help` for available serial commands.

## BLE Provisioning

The firmware advertises as `PowerMonitor` with service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b`.

### Using Web Bluetooth (Chrome)

1. Open a Web Bluetooth compatible browser
2. Connect to `PowerMonitor`
3. Write JSON commands to the `cmd` characteristic (`c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4`)
4. Read responses from the `resp` characteristic (`d8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01`)

### Example commands

```json
// Set WiFi credentials
{"cmd":"set_wifi","ssid":"MyNetwork","pass":"MyPassword","pin":0}

// Set MQTT broker
{"cmd":"set_mqtt","broker":"192.168.1.100","port":1883,"topic":"power-monitor/data","pin":0}

// Set custom HTTP endpoint
{"cmd":"set_http","url":"https://api.example.com/v1/data","token":"Bearer abc123","enabled":true,"pin":0}

// Configure relay (overcurrent protection)
{"cmd":"set_relay","idx":0,"channel":0,"overcurrent_A":5.0,"trip_delay_ms":1000,"gpio_pin":25,"pin":0}

// Configure battery for SoC
{"cmd":"set_battery","channel":0,"capacity_mAh":5000.0,"initial_soc_pct":100.0,"pin":0}

// Set BLE security PIN
{"cmd":"set_pin","old_pin":0,"new_pin":123456}

// Reset coulomb counter
{"cmd":"reset_coulomb","channel":0,"pin":123456}

// Get status
{"cmd":"get_status","pin":123456}

// Factory reset (wipes all NVS settings)
{"cmd":"factory_reset","pin":123456}
```

See `docs/API.md` for the complete BLE command reference.

## Serial CLI Commands

Connect at 115200 baud:

| Command | Description |
|---|---|
| `status` | Show IP, log entries, coulomb mAh, battery SoC |
| `sensors` | Print all raw sensor readings |
| `relay status` | Show relay configs and current GPIO states |
| `relay N 0/1` | Manually set relay N OFF/ON |
| `reset coulomb N` | Reset coulomb counter for channel N (0-3) |
| `flush log` | Pop and discard buffered log entries |
| `help` | Show all commands |

## MQTT Topics

| Topic | Direction | Description |
|---|---|---|
| `power-monitor/data` | Publish | JSON sensor snapshot + metadata |
| `power-monitor/logbin` | Publish | Base64-encoded compressed log batches |

## Architecture

```
src/main.cpp
  |
  +-- 1s timer: read_sensors() -> log_sample() -> update_coulomb_counter() -> evaluate_relays()
  |
  +-- 5s timer: read_sensors() -> publish_data() -> update_display()
  |
  +-- loop: loop_connectivity() + loop_ble_provisioner() + handle_serial_cli()
```

| Module | File | Purpose |
|---|---|---|
| Sensor Manager | `src/sensor_manager.cpp` | I2C initialization and reading |
| Settings Manager | `src/settings_manager.cpp` | NVS persistence for all config |
| Data Logger | `src/data_logger.cpp` | Delta-compressed RAM logging |
| Coulomb Counter | `src/coulomb_counter.cpp` | mAh accumulation per channel |
| Relay Controller | `src/relay_controller.cpp` | Trip/reset logic with SoC |
| BLE Provisioner | `src/ble_provisioner.cpp` | GATT command interface |
| Connectivity | `src/connectivity_manager.cpp` | WiFi, MQTT, Blynk, HTTP |
| Display | `src/display_manager.cpp` | SSD1306 OLED page cycling |

## Build Targets

| Environment | Board | Display | Flash | RAM |
|---|---|---|---|---|
| `esp32dev` | Generic ESP32 | Yes | ~93% | ~29% |
| `esp32c3` | ESP32-C3 | Yes | ~87% | ~29% |
| `esp32c3_nodisplay` | ESP32-C3 | No | ~87% | ~29% |

Partition table: `min_spiffs.csv` (1.9MB app).

## Documentation

- `docs/API.md` — Complete API reference (BLE commands, MQTT payloads, data structures, logging format, NVS keys)
- `CLAUDE.md` — Developer guide for Claude Code interactions

## License

MIT
