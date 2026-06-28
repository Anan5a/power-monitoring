# Power Monitor v2

ESP32-based multi-channel power monitoring IoT device with pod/channel sensor abstraction, generic switch control, BLE provisioning, and host simulation.

## Features

- **Pod/channel sensor model** — mix DC (INA226) and AC (BL0939) sensors on one board
- **Multi-INA226 support** — up to 8 modules on a single I2C bus
- **BL0939 AC energy meter** — UART-based, 2 channels per chip, multiple chips supported
- **Generic switch controller** — relays, MOSFETs (low/high-side), SSRs, expander outputs
- **Auto-trip protection** — overcurrent, undervoltage, battery SoC thresholds with trip/reset delays
- **Delta-compressed data logging** — 16KB RAM ring buffer, LittleFS overflow fallback
- **Coulomb counting** — per-channel mAh accumulator with NVS persistence
- **Energy counting** — per-channel Wh accumulator
- **Connectivity** — WiFi, MQTT, custom HTTP endpoint, Supabase telemetry
- **Secure BLE provisioning** — PIN-protected JSON commands for all settings
- **OLED display** — 5-page cycling (conditional compilation)
- **UI manager** — debounced buttons, LED status indicators, display page cycling
- **Host simulation** — build and run firmware logic on Linux/macOS without hardware
- **Board abstraction** — single codebase for ESP32, ESP32-C3, and ESP32-S3

## Hardware Requirements

| Component | Purpose | Interface |
|---|---|---|
| ESP32 / ESP32-C3 / ESP32-S3 | Main MCU | — |
| INA226 (1–8 modules) | DC voltage/current/power | I2C (0x40–0x4F) |
| BL0939 (0–4 modules) | AC voltage/current/power | UART |
| SSD1306 0.96" OLED | Status display (optional) | I2C (0x3C) |
| Relays / MOSFETs / SSRs | Output control | GPIO |

## Quick Start

### 1. Build

```bash
# Default (ESP32-C3)
~/.platformio/venv/bin/pio run -e esp32c3

# ESP32-S3
~/.platformio/venv/bin/pio run -e esp32s3

# Classic ESP32
~/.platformio/venv/bin/pio run -e esp32dev
```

### 2. Flash

```bash
~/.platformio/venv/bin/pio run -e esp32c3 --target upload
```

### 3. Monitor

```bash
~/.platformio/venv/bin/pio device monitor
```

Type `help` for available serial commands.

### 4. Simulate (no hardware needed)

```bash
cd sim
make clean && make -j$(nproc)
./build/power_monitor_sim
```

## Documentation

| Document | Contents |
|---|---|
| `docs/ARCHITECTURE.md` | Pod/channel model, task timing, data flow |
| `docs/SENSORS.md` | INA226 and BL0939 configuration |
| `docs/SWITCHES.md` | Switch controller API and auto-trip logic |
| `docs/UI.md` | Button/LED configuration and events |
| `docs/SIMULATION.md` | Host simulation harness |
| `docs/API.md` | BLE commands, MQTT payloads, NVS keys |
| `CLAUDE.md` | Developer guide for Claude Code |

## Build Targets

| Environment | Board | Display | RAM | Flash |
|---|---|---|---|---|
| `esp32c3` | ESP32-C3-DevKitM-1 | Yes | 25% | 42% |
| `esp32s3` | ESP32-S3-DevKitC-1 | Yes | 27% | 41% |
| `esp32dev` | Generic ESP32 | Yes | 29% | 44% |
| `esp32c3_nodisplay` | ESP32-C3 | No | — | — |

## License

MIT
