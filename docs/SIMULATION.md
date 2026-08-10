# Simulation Harness

The `sim/` directory provides a host-native simulation that compiles the firmware's business logic as a Linux/macOS executable. It stubs out Arduino/ESP32 hardware APIs and provides mock sensor data generators, enabling fast iteration without real hardware.

## Quick Start

```bash
cd /home/sayem/sources/power-monitoring/sim
make clean && make -j$(nproc)
./build/power_monitor_sim
```

The simulator runs 60 iterations of the 1-second sensor loop, printing telemetry JSON to stdout every 5 seconds.

## Architecture

```
sim/
├── Makefile              # Build system (g++ on Linux/macOS)
├── CMakeLists.txt        # Alternative CMake build
├── main.cpp              # Sim entry point: runs setup() + loop() at 1s/10ms cadence
├── hal/                  # Arduino/ESP32 API stubs
│   ├── Arduino.h         # millis(), delay(), Serial, digitalWrite, pinMode, etc.
│   ├── arduino_stubs.cpp
│   ├── WiFi.h / wifi_stub.cpp        # Always "connected"
│   ├── Wire.h / wire_stub.cpp        # I2C mock
│   ├── Preferences.h / preferences_stub.cpp  # In-memory key/value store
│   ├── FS.h / LittleFS.h / littlefs_stub.cpp
│   ├── gpio_stub.h / gpio_stub.cpp   # GPIO state tracking
│   ├── SD.h / sd_stub.cpp            # SD card stub (no-op, for data_logger)
│   ├── SPI.h                         # SPI stub (no-op, for data_logger)
│   ├── ESP.h / esp_stub.cpp          # ESP.getFreeHeap(), getMinFreeHeap()
│   └── esp_system.h                  # esp_reset_reason() stub
├── mock/                 # Mock sensor data generators
│   ├── waveform_generator.h / .cpp   # Sine, ramp, step, noise waveforms
│   ├── ina226_mock.h / .cpp          # INA226 mock (registers as POD_INA226)
│   └── bl0939_mock.h / .cpp          # BL0939 mock (registers as POD_BL0939)
└── shims/                # Shim implementations for firmware modules
    ├── sensor_manager.h / .cpp        # Mock sensor manager with pod registration
    ├── connectivity_manager.h / .cpp  # Stub that prints JSON to stdout
    ├── connectivity_publish_shim.h / .cpp  # Slim shim for telemetry_build() deps
    └── telemetry_deps_stubs.cpp       # Stubs for device_identity, ble_provisioner,
                                       # ota_client, connectivity, event_log
```

## What the Sim Exercises

The simulator compiles and runs the **real firmware modules** from `../src/`:

- `data_logger.cpp` — delta-compressed ring buffer (with SD card stubs)
- `coulomb_counter.cpp` — mAh integration
- `energy_counter.cpp` — Wh integration
- `settings_manager.cpp` — NVS-backed settings (in-memory stub)

It uses **shim versions** of hardware-dependent modules:

- `sensor_manager.cpp` — registers mock INA226 and BL0939 pods that return synthetic waveforms
- `connectivity_manager.cpp` — prints telemetry JSON to stdout instead of sending over the network

The **test_publish_path** test additionally compiles `telemetry.cpp` against stubs for
`device_identity`, `ble_provisioner`, `ota_client`, and `connectivity_manager` to
validate the TelemetrySnapshot JSON shape end-to-end.

## Adding a New Mock Pod

1. Create `mock/my_sensor_mock.h` and `mock/my_sensor_mock.cpp`.
2. Implement a `pod_my_sensor_read(PodState* pod)` function that fills in voltage/current/power.
3. In `shims/sensor_manager.cpp`, call `register_pod(POD_MY_TYPE, "MySensor", 1, pod_my_sensor_read)`.
4. Add the `.cpp` to `SRCS` in `Makefile`.

## Adding a New HAL Stub

1. Create `hal/MyLib.h` with the function signatures you need.
2. Create `hal/my_lib_stub.cpp` with no-op or mock implementations.
3. Add the `.cpp` to `SRCS` in `Makefile`.

## Build Options

```bash
make                    # Build
make clean              # Clean build artifacts
make run                # Build and run
```

The Makefile defines `-DBOARD_ESP32C3=1` by default. Change to `-DBOARD_ESP32DEV=1` or `-DBOARD_ESP32S3=1` to test different board pinouts.

## Limitations

- No FreeRTOS scheduler — tasks run sequentially in `main.cpp`'s loop.
- No real I2C/SPI/UART — all sensor data is synthetic.
- No real WiFi/MQTT/Supabase — telemetry goes to stdout.
- No real GPIO — button/LED state is tracked in memory.
- Good for: algorithm iteration, data format changes, cloud payload testing.
- Not good for: timing-sensitive bugs, interrupt handling, peripheral driver development.
