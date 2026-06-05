# Firmware Issues

Audit date: 2026-06-05
Files: src/*.cpp, include/*.h, include/config.h

## Critical Bugs

### 1. Timestamp formula corrupts `recorded_at` (src/connectivity_manager.cpp:889)

```cpp
time_t epoch_s = (epoch_time > 0) ? epoch_time + ms / 1000 : time(nullptr);
```

Adds uptime milliseconds to epoch time. If device runs 56 days before NTP sync, `ms/1000 ≈ 5M seconds` and `recorded_at` jumps 56 days into future. Server-side fallback now clamps bad timestamps to `now()`, but fix firmware:
```cpp
time_t epoch_s = (epoch_time > 0) ? epoch_time : time(nullptr);
```

### 2. `xTaskCreatePinnedToCore` uses `tskNO_AFFINITY` (src/main.cpp)

Both `networkTask` and `sensorTask` call `xTaskCreatePinnedToCore` but pass `tskNO_AFFINITY` as core argument. This means they are NOT pinned to any core — the kernel schedules them wherever. Comments say "pinned to Core 0" / "pinned to Core 1" but this is not happening. Should use `0` or `1` instead of `tskNO_AFFINITY` to actually pin.

### 3. Coulomb counter polarity — shunt voltage sign unreliable (src/coulomb_counter.cpp)

Direction detection via `ina3221_getShuntVoltage(ch)` checks instantaneous shunt voltage to decide charge vs discharge. If shunt resistor sense polarity is reversed or load briefly back-feeds, sign flips and mAh goes backward. Should use `battery_power` sign from trigger-side logic, or debounce direction changes with hysteresis.

### 4. Relay GPIO pins unvalidated (src/relay_controller.cpp)

`pinMode()` / `digitalWrite()` called on GPIO numbers loaded from NVS. If NVS is corrupted or stores an invalid GPIO (e.g., 3=RX, 1=TX on ESP32-C3), the call succeeds but pin doesn't exist — relay silently fails.

### 5. `uint16_t dt_ms` in DeltaEntry limits gap (include/data_logger.h)

`dt_ms` is uint16_t → max 65535 ms ≈ 65 seconds. Code checks for >60s gaps to force BaseEntry, but if any gap slips through (e.g., timer delay), delta reconstruction produces wrong timestamps.

## Logic Flaws

### 6. `fast_sensor_update` is always false (src/main.cpp)

`networkTask` loop reads `g_sensor_queue` with `portMAX_DELAY`, but nothing ever queues to `g_sensor_queue`. The fast sensor path is dead code.

### 7. Energy counter uses `power * dt` without dt integration (src/energy_counter.h)

Assumes 1-second granularity. If `update_energy_counter()` is called at irregular intervals (e.g., after a blocking WiFi reconnect), the Wh values are wrong because the loop uses fixed `SAMPLE_INTERVAL_MS` but the actual elapsed time varies.

### 8. Relay auto mode defaults to OFF (src/relay_controller.cpp:36)

`relay_auto_enabled = false` means relays never trip automatically until someone sends a BLE command to enable it. On power loss recovery, a device could over-discharge the battery indefinitely.

### 9. `publish_data_supabase` sends `p_recorded_at` as raw uptime-scaled epoch (src/connectivity_manager.cpp:989)

```cpp
elem["p_recorded_at"] = (uint32_t)epoch_s;
```

Even if timestamp formula is fixed, this sends `recorded_at` as a separate field from `recorded_at` in the insert_telemetry RPC. If `p_recorded_at` is wrong (see bug #1), the server-side fallback doesn't apply because the RPC only validates `p_recorded_at` — not `recorded_at` from the trigger.

### 10. `publish_log_batch_supabase` uses `recorded_at` from saved log timestamps (src/connectivity_manager.cpp:645+)

Log buffer `recorded_at` was already baked when log was written. If timestamp was wrong at write time (bug #1), log replay sends wrong timestamps via `insert_telemetry` RPC. Server-side fallback covers this now, but data logged during bad-timestamp window is permanently wrong.

## Resilience & Safety

### 11. No watchdog timer

If any operation hangs (WiFi connect, NTP sync, I2C bus stall), the device locks up permanently until power cycle. Add `esp_task_wdt_init()` / `esp_task_wdt_add()` or use `#include "esp_task_wdt.h"`.

### 12. NTP sync blocks indefinitely (src/connectivity_manager.cpp)

`configTime()` + `wait for time to be set` loop has no timeout. If NTP server is unreachable, device hangs in `setup()` forever. Add `timeout` with fallback to RTC or compile-time default.

### 13. WiFi connect blocks for 10s (src/connectivity_manager.cpp)

`WiFi.begin()` + 10s timeout is better than infinite, but 10s is synchronous in the networkTask loop — all other network operations (BLE, MQTT, Supabase) are stalled during this.

### 14. LittleFS has no wear leveling

SPIFFS was replaced by LittleFS but both lack wear-leveling. Log overflow file is re-written frequently. On a device writing overflow data for months, flash cells may wear out. No monitoring.

### 15. Heap fragmentation risk (src/connectivity_manager.cpp)

Repeated `new WiFiClientSecure` / `new HTTPClient` for each HTTP call. Combined with BLE stack (~50KB), JSON documents, and large stack for both tasks, heap can fragment. Device may crash after days/weeks.

### 16. MQTT max packet size 1024 (src/connectivity_manager.cpp)

`#define MQTT_MAX_PACKET_SIZE 1024` limits MQTT payload to ~1KB. Batched log data can exceed this silently. PubSubClient silently truncates oversized publishes.

### 17. BLE advertising pin mismatch not handled (src/ble_provisioner.cpp)

If BLE PIN was changed via BLE but Supabase sync failed, the device is unreachable over BLE until factory reset. No fallback to allow PIN-less auth if device has no known user.

## Code Quality

### 18. Duplicate `#include "coulomb_counter.h"` (src/ble_provisioner.cpp:9)

Included twice — lines 7 and 9.

### 19. `publish_relay_state` declared twice in header (include/connectivity_manager.h)

Function appears twice — intentional but confusing.

### 20. Two HTTP client patterns (src/connectivity_manager.cpp)

`g_telemetry_client` / `g_telemetry_http` — reused (setReuse=true). `g_supa_client` / `g_supa_http` — freshly allocated per call. Inconsistent. The reused client leaks DNS/connection handles over time.

### 21. Large static buffers on stack (src/connectivity_manager.cpp)

`char buffer[4096]` and `char full_url[512]` on stack in multiple functions. With FreeRTOS task stack defaults (~3-4KB), this risks stack overflow. Should use static or heap buffers.

### 22. `strftime` buffer size hardcoded (src/data_logger.cpp + connectivity_manager.cpp)

`strftime(buf, sizeof(buf), ...)` used with hardcoded buffers. If firmware locale changes date format, buffer may truncate silently.

## Configuration & Build

### 23. I2C pins conflict between ESP32 and ESP32-C3 (include/config.h)

Pins defined as SDA=5, SCL=6 for C3, but default target in platformio.ini is `esp32dev` (original ESP32). On original ESP32, GPIO5 and GPIO6 are not I2C-capable (need 21/22). Same build for both boards will fail on standard ESP32.

### 24. ESP32-C3 GPIO 20/21 may conflict (include/config.h)

GPIO20 and GPIO21 for relays. On ESP32-C3, GPIO20 is flash chip CS and GPIO21 is flash clock. Using these for relays may cause flash corruption during writes.

### 25. `HAS_DISPLAY` is 1 by default (include/config.h)

C3_nodisplay variant sets `-DHAS_DISPLAY=0`, but if any env omits the flag, display code compiles in and crashes on C3 boards without OLED.

### 31. Sensor-data queue is never consumed (src/main.cpp + src/core_shared.cpp)

`sensorTask` pushes to `g_sensor_queue` via `push_sensor_data()` every 1s. `networkTask` tries to read from it with `portMAX_DELAY`, but the queue is only 16 slots deep. When networkTask is busy (WiFi reconnect), queue fills up and `xQueueSend` with 0 timeout silently drops data. Worse: networkTask's blocking read means it processes the latest entry at most once per iteration — the "fast update" path is dead code and has never worked.

### 32. `portMAX_DELAY` in networkTask blocks forever (src/main.cpp)

`xQueueReceive(g_sensor_queue, &data, portMAX_DELAY)` blocks until data arrives. If queue fills and drops entries (16 slots @1s each), the last entry stays unread and networkTask unblocks. But `portMAX_DELAY` means it waits forever if queue is empty — acceptable since sensorTask pushes every 1s, but any queue bug (e.g., `xQueueCreate` failure) hangs the entire networkTask permanently.

### 33. Energy counter uses virtual channel config for power (src/energy_counter.cpp:22-26)

`update_energy_counter()` calls `get_sensor_voltage()` / `get_sensor_current()` which are declared in `connectivity_manager.h` but defined in `connectivity_manager.cpp`. These rely on WiFi/NTP state and may return 0 if network is down. Energy counter should use raw INA readings directly, not virtual channel selectors from the connectivity manager.

### 34. Energy counter ignores `sensor_data.ina226_power` for vc=3 (src/energy_counter.cpp:32)

Channel 3 always uses `data.ina226_power` even if `settings_load_virtual_channel(3, ...)` returns true. Virtual channel config for channel 3 is silently ignored. Energy counter and virtual channel logic are inconsistent.

### 35. `total_power` formula uses `ads1115_volts` instead of `ina3221_busV` (src/main.cpp:122, 354)

```cpp
total_power += data.ads1115_volts[ch] * data.ina3221_current[ch];  // lines 122, 354
```

Uses `ads1115_volts` (0-3.3V ADC range, has voltage divider) instead of `ina3221_busV` (direct bus voltage). Produces meaningless power on OLED display and CLI `sensors` command. Should be `data.ina3221_busV[ch]`. Bug exists in two places: sensorTask display update and CLI sensor print.

### 36. `print_sensor_data` shows `ads1115_volts` labeled as CH voltage (src/main.cpp:20)

```cpp
Serial.printf("CH%d: %.2fV %.3fA (cal)\n", i, data.ads1115_volts[i], data.ina3221_current[i]);
```

Shows 0-3.3V ADC readings labeled as "CH" voltage with "(cal)" suffix. Same bug as #35 — should use `data.ina3221_busV[i]`. This misleads debugging.

### 37. `print_status` SoC formula uses raw mAh without energy_Wh (src/main.cpp:39)

```cpp
soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
```

Only considers coulomb counter mAh but ignores energy counter Wh. Both counters track different channels and different physical quantities. SoC display is partial and may be misleading.

## Performance

### 26. `handle_serial_cli` runs in `loop()` on Core 1

At 115200 baud, serial CLI blocks the loop iteration. `loop()` runs `delay(10)` — serial data at high baud rates can queue up and cause noticeable latency in the sensor task.

### 27. OLED display in sensorTask (src/main.cpp)

`update_display()` runs in the 1-second sensorTask loop. SSD1306 I2C communication (~10ms per page) delays sensor reads slightly. Display should move to networkTask or its own dedicated task.

### 28. `ArduinoJson` serialize/deserialize overhead

`publish_data_supabase()` builds a full JSON document per call (~1KB). For 1-second logging rate, this uses significant CPU. DynamicJsonDocument allocations fragment heap.

## Monitoring

### 29. No connection quality metrics

RSSI, reconnect count, HTTP error rates are not tracked. Debugging intermittent WiFi/MQTT/Supabase failures requires serial console access.

### 30. No firmware version reporting

No compile-time `GIT_REVISION` or `VERSION` string in telemetry metadata. Impossible to tell which firmware build produced a given row in the database.