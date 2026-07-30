# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32 power-monitoring IoT project using PlatformIO (Arduino framework).

**v2 features:** coulomb counting per channel, delta-compressed 1-second data logging (RAM-first, SPIFFS fallback), NVS-persisted relay thresholds/settings/calibration, secure BLE provisioning/command interface, battery SoC-based relay logic, cycling OLED display pages, and optional custom HTTP endpoint publishing.

## Common Commands

PlatformIO CLI is installed at `~/.platformio/venv/bin/pio`.

Build default target (`esp32dev`):
```bash
~/.platformio/venv/bin/pio run
```

Build for a specific board:
```bash
~/.platformio/venv/bin/pio run -e esp32dev
~/.platformio/venv/bin/pio run -e esp32c3
~/.platformio/venv/bin/pio run -e esp32c3_nodisplay
```

Upload (auto-detects port):
```bash
~/.platformio/venv/bin/pio run --target upload -e esp32dev
```

Upload to a specific port:
```bash
~/.platformio/venv/bin/pio run --target upload -e esp32dev --upload-port /dev/ttyUSB0
```

Monitor serial output:
```bash
~/.platformio/venv/bin/pio device monitor
```

Build + upload + monitor:
```bash
~/.platformio/venv/bin/pio run -e esp32dev --target upload && ~/.platformio/venv/bin/pio device monitor
```

Clean:
```bash
~/.platformio/venv/bin/pio run --target clean -e esp32dev
```

Install/update libraries:
```bash
~/.platformio/venv/bin/pio pkg update
~/.platformio/venv/bin/pio pkg install --library "owner/name@version"
```

## Supported Boards

`platformio.ini` defines three environments:
- `esp32dev` — generic ESP32 (gen 1) dev board (xtensa). Default target.
- `esp32c3` — generic ESP32-C3 supermini (RISC-V). Uses `esp32-c3-devkitm-1` board definition. BLE works on C3.
- `esp32c3_nodisplay` — ESP32-C3 without OLED display libs (saves flash).

If you add a new product, create a new `[env:...]` section with the appropriate `board` and `lib_deps`.

## Architecture

**Entry point:** `src/main.cpp` (Arduino `setup()`/`loop()`).

**Module layout:**
- `include/config.h` — Compile-time constants: WiFi/MQTT/Blynk defaults, I2C pins/addresses, sample intervals, BLE UUIDs, relay GPIOs, log buffer sizes. These are `#define` macros; edit before flashing. Most settings can also be changed at runtime via BLE provisioning.
- `src/sensor_manager.cpp` + `include/sensor_manager.h` — Initializes the shared `Wire` bus and all I2C sensors. `read_sensors()` returns a `SensorData` struct with arrays for INA3221 (3 channels), INA226 (1 channel), and ADS1115 (4 channels).
- `src/settings_manager.cpp` + `include/settings_manager.h` — NVS persistence via `Preferences`. Stores WiFi/MQTT/HTTP credentials, relay rules, battery configs, calibration, coulomb counts, BLE PIN, factory reset. All other modules depend on this.
- `src/data_logger.cpp` + `include/data_logger.h` — 32KB RAM circular buffer with delta compression (BaseEntry + DeltaEntry). SPIFFS fallback when buffer is full and WiFi is disconnected. `log_pop_batch()` retrieves data for transmission.
- `src/coulomb_counter.cpp` + `include/coulomb_counter.h` — Per-channel mAh accumulator. Loaded from NVS on boot, persisted every 5 minutes.
- `src/relay_controller.cpp` + `include/relay_controller.h` — Advanced relay logic with trip/reset delays and hysteresis. Supports overcurrent, undervoltage, and battery SoC thresholds. Relay configurations are loaded from NVS; defaults are created on first boot.
- `src/ble_provisioner.cpp` + `include/ble_provisioner.h` — BLE GATT server with secure JSON command interface. PIN-protected commands for provisioning WiFi, MQTT, HTTP, relay thresholds, battery config, coulomb reset, factory reset. See `docs/API.md` for full command reference.
- `src/connectivity_manager.cpp` + `include/connectivity_manager.h` — Manages WiFi (loads credentials from NVS first, falls back to compile-time defaults), MQTT (PubSubClient + ArduinoJson), Blynk IoT, optional custom HTTP endpoint, and BLE sensor data notify. `publish_data()` includes log metadata and triggers HTTP publish if enabled.
- `src/display_manager.cpp` + `include/display_manager.h` — SSD1306 OLED, 5-page cycling display: status page (IP, total power, log entries) + per-channel pages (V, I, P, SoC%). Conditional compilation via `#if HAS_DISPLAY`.

**Data flow:**
```
1s timer: read_sensors() -> log_sample() -> update_coulomb_counter() -> evaluate_relays()
5s timer: read_sensors() -> publish_data() -> update_display()
loop:     loop_connectivity() + loop_ble_provisioner() + handle_serial_cli()
```

**Timing:**
- 1-second timer: sensor reads, logging, coulomb counting, relay evaluation
- 5-second timer (`SAMPLE_INTERVAL_MS`): network publish (MQTT + HTTP + Blynk), display refresh
- `loop_connectivity()` and `loop_ble_provisioner()` run on every loop tick with 10ms delay

**I2C wiring:**
- Default SDA=GPIO21, SCL=GPIO22. Change in `config.h` if needed.
- All devices share one bus. Addresses are configured in `config.h`:
  - INA3221: `0x40`
  - INA226: `0x41`
  - ADS1115: `0x48`
  - OLED: `0x3C`
- If you add more sensors, verify addresses are unique to avoid bus collisions.

**Relay GPIO defaults:**
- Relay 1: GPIO25
- Relay 2: GPIO26
- Relay 3: GPIO27
- Relay 4: GPIO14

**Dependencies** (declared in `platformio.ini`):
- `PubSubClient` — MQTT
- `ArduinoJson` — JSON serialization
- `Adafruit INA3221 Library` — 3-channel sensor
- `INA226` (RobTillaart) — single-channel sensor
- `Adafruit ADS1X15` — 16-bit ADC
- `Adafruit SSD1306` + `Adafruit GFX Library` — OLED display (conditional on `HAS_DISPLAY`)
- `Blynk` — IoT dashboard

## Serial CLI

At `115200 baud`, type commands:
- `status` — IP, log entries, coulomb mAh, SoC
- `sensors` — Raw readings
- `relay status` — Config and GPIO states
- `relay N 0/1` — Manual override
- `reset coulomb N` — Reset channel N counter
- `flush log` — Empty RAM buffer
- `help` — Command list

## Important Notes

- WiFi connection in `init_connectivity()` is blocking. If credentials are wrong or AP is unreachable, `setup()` hangs. On first boot, ensure compile-time defaults in `config.h` are correct, or provision via BLE afterward.
- MQTT reconnection is handled lazily in `loop_connectivity()`.
- Blynk is initialized with `Blynk.config()` after WiFi is connected. If Blynk server is unreachable, init logs a failure but does not block indefinitely.
- BLE uses the built-in ESP32 Arduino BLE library. No extra `lib_deps` required. Works on both classic ESP32 and ESP32-C3.
- There is only ONE BLE server in the firmware (in `ble_provisioner.cpp`). The `connectivity_manager` calls `ble_notify_sensor_data()` to push live data; it does not create its own BLE server.
- `board_build.partitions = partitions_ota_4m.csv` provides two 1.9 MB OTA slots (2 × 1.9 MB on 4 MB flash, 2 × 3.7 MB on 8 MB flash). SPIFFS is only used for log overflow fallback when network is unavailable.
- `CORE_DEBUG_LEVEL=3` enables ESP32 debug logs. Lower to `0` for release builds to save flash and reduce serial noise.
- Log buffer is 32KB RAM (not 180KB as originally planned) because the Arduino framework + static data + BLE stack consumes significant DRAM. On ESP32 with 320KB RAM, this leaves ~220KB for heap/stack.

## Documentation

See `docs/API.md` for comprehensive interface documentation: BLE commands, MQTT payloads, HTTP endpoint, data structures, logging format, NVS keys, relay logic, and build instructions.
