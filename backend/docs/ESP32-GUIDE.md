# ESP32 Arduino — Supabase Telemetry Client

This guide shows how to add Supabase telemetry posting to any ESP32 Arduino project.

## Prerequisites

- ESP32 with WiFi connectivity
- [ArduinoJson](https://arduinojson.org/) library
- [HTTPClient](https://github.com/espressif/arduino-esp32/tree/master/libraries/HTTPClient) (included in ESP32 core)

## Payload Format

Every 5 minutes, the ESP32 POSTs a batch with this structure:

```json
{
  "p_device_key": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "p_payload": {
    "ina3221_v0": 12.34,
    "ina3221_i0": 1.23,
    "ina3221_v1": 12.34,
    "ina3221_i1": 0.45,
    "ina3221_v2": 12.34,
    "ina3221_i2": 0.67,
    "ina226_v": 5.01,
    "ina226_i": 2.10,
    "ina226_p": 10.52,
    "ads1115_0": 1.234,
    "ads1115_1": 2.345,
    "ads1115_2": 3.456,
    "ads1115_3": 4.567,
    "coulomb_mah0": 1234.5,
    "coulomb_mah1": 567.8,
    "coulomb_mah2": 89.1,
    "coulomb_mah3": 0.0,
    "soc_pct0": 85.2,
    "soc_pct1": 72.1,
    "soc_pct2": 95.0,
    "soc_pct3": 0.0,
    "relay_states": 0,
    "log_entries": 42,
    "log_overflow": false,
    "log_overflow_bytes": 0
  },
  "p_metadata": {
    "rssi": -55,
    "vcc": 3.31,
    "uptime_s": 3600
  }
}
```

## Complete ESP32 Client

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// --- Configuration (set via BLE or hardcode) ---
static const char* WIFI_SSID = "YourSSID";
static const char* WIFI_PASS = "YourPassword";
static const char* SUPABASE_URL = "https://your-project.supabase.co";
static const char* SERVICE_ROLE_KEY = "eyJhbGc..."; // service_role key
static const char* DEVICE_KEY = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";

// --- Batch buffer (stores last 60 readings = 1 hour at 1/min) ---
static const int BATCH_SIZE = 60;
static int payloadBuffer[BATCH_SIZE][16]; // simplified — use struct for real data
static int bufferIndex = 0;
static unsigned long lastPostMs = 0;
static const unsigned long POST_INTERVAL_MS = 300000; // 5 minutes

void sendTelemetryBatch() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin(String(SUPABASE_URL) + "/rest/v1/rpc/insert_telemetry");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SERVICE_ROLE_KEY);
    http.addHeader("Authorization", "Bearer " + String(SERVICE_ROLE_KEY));
    http.addHeader("Prefer", "return=minimal");

    // Build payload
    JsonDocument doc;
    doc["p_device_key"] = DEVICE_KEY;

    JsonObject payload = doc["p_payload"].to<JsonObject>();
    // Add sensor readings from your buffer
    // Example: payload["ina3221_v0"] = sensorValue;

    JsonObject metadata = doc["p_metadata"].to<JsonObject>();
    metadata["rssi"] = WiFi.RSSI();
    metadata["vcc"] = analogRead(VP) / 4095.0 * 3.3; // example
    metadata["uptime_s"] = millis() / 1000;

    String body;
    serializeJson(doc, body);

    int httpCode = http.POST(body);
    if (httpCode != 200 && httpCode != 201 && httpCode != 204) {
        Serial.printf("Supabase error %d\n", httpCode);
        // Store in SPIFFS for retry later
    } else {
        Serial.println("Telemetry sent");
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    // ... wait for connection ...
}

void loop() {
    unsigned long now = millis();

    // Sample sensors every second
    static unsigned long lastSampleMs = 0;
    if (now - lastSampleMs >= 1000) {
        lastSampleMs = now;
        // readSensors() — your sensor reading logic
        // buffer current reading
    }

    // Post batch every 5 minutes
    if (now - lastPostMs >= POST_INTERVAL_MS) {
        lastPostMs = now;
        sendTelemetryBatch();
    }
}
```

## BLE Provisioning Keys

The ESP32 firmware accepts these BLE commands to configure Supabase:

| Command | Fields | Description |
|---|---|---|
| `set_supabase` | `url`, `service_role_key`, `device_key` | Configure Supabase endpoint |
| `get_status` | — | Returns device_id, entries, overflow status |

See `docs/API.md` for full BLE command reference.

## Offline Handling

If the ESP32 cannot reach Supabase:

1. **RAM buffer** — Store readings in a circular buffer (already implemented in your firmware)
2. **SPIFFS fallback** — When RAM is full and network is down, write to SPIFFS (already implemented)
3. **Retry on reconnect** — `log_flush_overflow()` sends SPIFFS batch when WiFi reconnects

## Testing with curl

```bash
curl -X POST "https://your-project.supabase.co/rest/v1/rpc/insert_telemetry" \
  -H "Content-Type: application/json" \
  -H "apikey: YOUR_SERVICE_ROLE_KEY" \
  -H "Authorization: Bearer YOUR_SERVICE_ROLE_KEY" \
  -d '{
    "p_device_key": "test-device-key",
    "p_payload": {"temperature": 25.3, "humidity": 60.1},
    "p_metadata": {"rssi": -50}
  }'
```

## Adding to Power Monitor Firmware

If you're using the power-monitor firmware from this repo:

1. The `publish_data_http()` function in `connectivity_manager.cpp` already sends HTTP POST
2. Modify it to call the Supabase RPC endpoint instead of a generic HTTP endpoint
3. Add `set_supabase` BLE command to store `supabase_url` + `service_role_key` + `device_key` in NVS
4. Build the full `p_payload` with all sensor fields from `SensorData` struct

The Supabase URL format is: `https://<project>.supabase.co/rest/v1/rpc/insert_telemetry`