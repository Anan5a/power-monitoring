# Firmware Architecture

## Overview

The power-monitoring firmware has been redesigned around a **pod/channel model** that decouples physical sensor hardware from logical measurement channels. This enables mixing DC (INA226) and AC (BL0939) sensors on the same board, with a unified data pipeline for logging, cloud publishing, and relay/switch control.

## Core Data Model

### `SensorSnapshot` (`include/sensor_pod.h`)

The central data structure passed between tasks:

```
SensorSnapshot
├── timestamp_ms
├── num_pods
├── pods[] (PodState)
│   ├── id
│   ├── type (POD_INA226, POD_BL0939)
│   ├── name
│   ├── num_channels
│   └── channels[] (PhysicalChannel)
│       ├── pod_id
│       ├── pod_channel
│       ├── voltage (V)
│       ├── current (A)
│       ├── power (W)
│       ├── energy_Wh
│       ├── coulomb_mAh
│       └── meta (stddev, spike)
└── total_logical_channels
```

A **Pod** is a physical measurement unit (e.g., one INA226 module, one BL0939 chip). A **Logical Channel** is a flat index across all pods, used by logging, counters, and cloud APIs.

### Accessor functions

```cpp
// By logical channel index (0..total_logical_channels-1)
float get_channel_voltage(uint8_t ch);
float get_channel_current(uint8_t ch);
float get_channel_power(uint8_t ch);

// From a specific snapshot (for queued/batched data)
float get_channel_voltage(const SensorSnapshot& snap, uint8_t ch);
float get_channel_current(const SensorSnapshot& snap, uint8_t ch);
float get_channel_power(const SensorSnapshot& snap, uint8_t ch);
```

## Module Map

```
┌─────────────────────────────────────────────────────────────┐
│  main.cpp                                                   │
│  ├── networkTask (Core 0, 10ms) — WiFi, MQTT, Supabase,    │
│  │   BLE, display, UI status                                │
│  ├── sensorTask (Core 1, 1s) — read_sensors(), logging,    │
│  │   coulomb/energy, evaluate_switches()                    │
│  ├── uiTask (Core 1, 50ms) — buttons, LEDs                 │
│  └── loop() — serial CLI                                   │
├─────────────────────────────────────────────────────────────┤
│  sensor_manager.cpp + sensor_pod.h                          │
│  ├── register_pod(type, name, channels, read_fn)            │
│  ├── init_sensors() — Wire, INA226, BL0939, legacy INA3221 │
│  └── read_sensors() → SensorSnapshot                        │
├─────────────────────────────────────────────────────────────┤
│  switch_controller.cpp + switch_controller.h                │
│  ├── init_switches() — load from NVS or create defaults     │
│  ├── evaluate_switches(snapshot) — auto trip/reset          │
│  ├── switch_set(idx, on) — manual latch                     │
│  └── switch_pulse(idx, ms) — press-and-hold                 │
├─────────────────────────────────────────────────────────────┤
│  ui_manager.cpp + ui_manager.h                               │
│  ├── init_ui() — pin modes, button/LED setup                │
│  ├── loop_ui() — debounce, LED patterns, events            │
│  └── ui_next_display_page() — page cycling hook            │
├─────────────────────────────────────────────────────────────┤
│  connectivity_manager.cpp — WiFi, MQTT, HTTP, Supabase      │
│  data_logger.cpp — 16KB delta-compressed ring buffer        │
│  coulomb_counter.cpp — mAh integration                      │
│  energy_counter.cpp — Wh integration                         │
│  ble_provisioner.cpp — NimBLE GATT command server           │
│  display_manager.cpp — SSD1306 5-page OLED                  │
│  settings_manager.cpp — NVS persistence                     │
└─────────────────────────────────────────────────────────────┘
```

## FreeRTOS Task Timing

| Task | Core | Tick | Work |
|---|---|---|---|
| `networkTask` | 0 | 10ms | WiFi, MQTT, Supabase, BLE, display, UI status |
| `sensorTask` | 1 | 1s | `read_sensors()`, logging, coulomb/energy, `evaluate_switches()` |
| `uiTask` | 1 | 50ms | `loop_ui()`, button debounce, LED patterns |
| `loop()` | 1 | 10ms | Serial CLI, heap check |

## Data Flow

```
sensorTask (1s)
  read_sensors() → SensorSnapshot
    ├── push_sensor_data() → g_sensor_queue → networkTask
    ├── log_sample() → 16KB RAM ring buffer
    ├── update_coulomb_counter()
    ├── update_energy_counter()
    └── evaluate_switches()

networkTask (10ms)
  ← g_sensor_queue
    ├── publish_data() → MQTT + HTTP + BLE notify
    ├── publish_data_supabase() → Supabase RPC
    └── update_display() (every 5s)
  publish_log_batch_supabase() (drains RAM + LittleFS)
  check_settings_commands() (every 5s)
  try_sync_epoch_time() (every 60s)
```
