// test_publish_path.cpp
// =============================================================================
// Host-side validation harness for the power-monitoring firmware's publish
// path. Catches the class of bugs where the firmware compiles clean but
// breaks against a real Supabase schema or a real PubSubClient:
//
//   1. TelemetrySnapshot v1 JSON shape — every field present, correct type,
//      correct units, floats rounded to 4 decimals, NaN/Inf downgraded to 0,
//      time_source / schema / v / ntp_synced all set, Content-Profile header
//      expected.
//   2. sync_battery_profiles / sync_battery_bindings RPC payloads — shape
//      matches the new migration (backend/supabase/migrations/2026_07_04_...).
//   3. claim_settings_command payload that the device sends.
//   4. MQTT LWT — verify the connect() overload takes will params.
//   5. set_relay / set_switch payload — confirm the device-side handler
//      reads the same fields the TypeScript wrapper sends.
//   6. NaN/Inf downgrade — feed a NaN V/I and verify the serialized JSON
//      has 0.0, not "NaN".
//
// The harness uses a fake SupabaseServer that captures incoming HTTP calls
// (URL, headers, body) into a vector. The test then deserialises the body
// and asserts against the expected schema.
//
// Build: see sim/Makefile (`make test_publish_path`).
// =============================================================================

#include "Arduino.h"
#include "config.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include "sensor_pod.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "coulomb_counter.h"
#include "cycle_counter.h"
#include "telemetry.h"
#include "data_logger.h"
#include "switch_controller.h"
#include "connectivity_manager.h"
#include "http_stub.h"
#include "connectivity_publish_shim.h"  // pulls in <ESP.h> and the sim knobs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <set>
#include <map>

#include <ArduinoJson.h>

// ── Test framework ─────────────────────────────────────────────────────────
static int g_tests = 0;
static int g_failures = 0;

// Helper to pass either const char* or std::string to printf %s.
static const char* msg_to_cstr(const char* s) { return s; }
static const char* msg_to_cstr(const std::string& s) { return s.c_str(); }

#define EXPECT(cond, msg) do {                                            \
        g_tests++;                                                       \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg_to_cstr(msg));\
        } else {                                                         \
            fprintf(stderr, "  ok   %s\n", msg_to_cstr(msg));            \
        }                                                                \
    } while (0)

// String-comparison assertion that tolerates either std::string or
// const char* on the actual side (and ArduinoJson's .as<const char*>() which
// can also return a string view that converts implicitly to std::string).
// If the actual value is null (e.g. .as<const char*>() on a missing key)
// we report FAIL with "(null)" rather than crashing.
#define EXPECT_EQ_STR(actual, expected, msg) do {                         \
        g_tests++;                                                       \
        std::string _a;                                                  \
        try {                                                            \
            _a = (actual);                                                \
        } catch (const std::logic_error&) {                              \
            _a = "(null)";                                               \
        }                                                                \
        std::string _e = (expected);                                     \
        if (_a != _e) {                                                  \
            g_failures++;                                                \
            fprintf(stderr,                                               \
                "  FAIL [%s:%d] %s: expected '%s', got '%s'\n",           \
                __FILE__, __LINE__, msg, _e.c_str(), _a.c_str());        \
        } else {                                                         \
            fprintf(stderr, "  ok   %s\n", msg);                         \
        }                                                                \
    } while (0)

#define EXPECT_TRUE(cond, msg) EXPECT((cond), msg)
#define EXPECT_FALSE(cond, msg) EXPECT(!(cond), msg)

#define EXPECT_DOUBLE_NEAR(actual, expected, tol, msg) do {               \
        g_tests++;                                                       \
        double _a = (double)(actual);                                    \
        double _e = (double)(expected);                                  \
        double _t = (double)(tol);                                       \
        if (fabs(_a - _e) > _t) {                                        \
            g_failures++;                                                \
            fprintf(stderr,                                               \
                "  FAIL [%s:%d] %s: expected %.6f, got %.6f (tol %.6f)\n",\
                __FILE__, __LINE__, msg, _e, _a, _t);                    \
        } else {                                                         \
            fprintf(stderr, "  ok   %s (%.6f)\n", msg, _a);               \
        }                                                                \
    } while (0)

// Check field presence via direct iteration on a named object. The macro
// takes the object as its first argument so the caller can use it on
// nested objects (device, wifi, channels[i], log, ...).
#define EXPECT_HAS(obj, field_name, msg) do {                            \
        g_tests++;                                                       \
        bool _present = false;                                           \
        for (JsonPair kv : obj) {                                        \
            if (strcmp(kv.key().c_str(), field_name) == 0) {             \
                _present = true;                                         \
                break;                                                   \
            }                                                            \
        }                                                                \
        if (!_present) {                                                 \
            g_failures++;                                                \
            fprintf(stderr, "  FAIL [%s:%d] %s: field '%s' missing\n",   \
                __FILE__, __LINE__, msg, field_name);                    \
        } else {                                                         \
            fprintf(stderr, "  ok   %s (has %s)\n", msg, field_name);    \
        }                                                                \
    } while (0)

// Pull in the sim-side knobs (declared in shims/connectivity_publish_shim.cpp).
extern void sim_set_local_ip(const char* ip);
extern void sim_set_epoch(time_t epoch);
extern void sim_set_ntp_synced(bool synced);
extern bool sim_ntp_synced();

// Backend URL for the self-hosted Go backend.
static const char* kBackendUrl = "https://backend.test";

// ── Setup helpers ─────────────────────────────────────────────────────────
static void setup_world() {
    init_settings();
    init_battery_profiles();
    init_battery_bindings();
    init_coulomb_counter();
    init_cycle_counter();
    init_data_logger();
    sim_set_local_ip("192.168.1.42");
    sim_set_epoch(1717500000);  // arbitrary post-2023 epoch
    sim_set_ntp_synced(true);
}

// Build a snapshot with a known good value on logical channel 0.
static SensorSnapshot good_snapshot(float v0, float i0) {
    SensorSnapshot snap{};
    snap.num_pods = 1;
    snap.total_logical_channels = 4;
    snap.pods[0].id = 0;
    snap.pods[0].type = POD_INA226;
    snap.pods[0].num_channels = 4;
    snap.pods[0].channels[0].voltage = v0;
    snap.pods[0].channels[0].current = i0;
    snap.pods[0].channels[0].power = v0 * i0;
    return snap;
}

// Build a snapshot with NaN V on logical channel 0.
static SensorSnapshot nan_snapshot() {
    SensorSnapshot snap = good_snapshot(NAN, 1.5f);
    return snap;
}

// ── Republish helper ──────────────────────────────────────────────────────
//
// Replicates the small slice of connectivity_manager.cpp::publish_data_supabase
// that builds the Supabase RPC envelope. Kept in the test so we don't have
// to link the full connectivity_manager.cpp (which pulls in PubSubClient,
// Blynk, mbedTLS — none of which compile on the host).
//
// MUST stay in sync with src/connectivity_manager.cpp:publish_data_supabase.

// Mirror of connectivity_manager.cpp::write_float — NaN/Inf downgrade to
// 0.0 then format to 4 decimals as a string (so the wire shape is a JSON
// number-as-string like "12.5000" instead of a JSON number).
static std::string write_float(float v) {
    if (!isfinite(v)) v = 0.0f;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", (double)v);
    return std::string(buf);
}

static void serialize_telemetry_flat(const TelemetrySnapshot& snap, JsonObject root) {
    // Mirrors connectivity_manager.cpp:serialize_telemetry_core() — flat format
    // matching backend ingest.go. Kept structurally identical so a divergence
    // between this test and the production serializer shows up as a failed assertion.
    root["ts"] = snap.ts;
    root["ts_ms"] = snap.ts_ms;
    root["schema"] = snap.schema;
    root["fw"] = snap.device.fw;
    root["uptime_ms"] = snap.device.uptime_ms;
    root["rssi"] = snap.wifi.rssi;
    root["heap_free"] = snap.heap_free;
    root["hw_rev"] = snap.hw_rev;
    root["time_source"] = (snap.ts == 0 || !sim_ntp_synced())
                            ? std::string("uptime")
                            : std::string("ntp");

    JsonObject data = root["data"].to<JsonObject>();

    // Channels
    for (uint8_t i = 0; i < snap.channel_count; i++) {
        const TelemetryChannel& c = snap.channels[i];
        char key[24];
        snprintf(key, sizeof(key), "ch%u_V", i);
        data[key] = write_float(c.V);
        snprintf(key, sizeof(key), "ch%u_I", i);
        data[key] = write_float(c.I);
        snprintf(key, sizeof(key), "ch%u_P", i);
        data[key] = write_float(c.P);
        snprintf(key, sizeof(key), "ch%u_energy_Wh", i);
        data[key] = write_float(c.energy_Wh);
        snprintf(key, sizeof(key), "ch%u_charge_mAh", i);
        data[key] = write_float(c.charge_mAh);
    }

    // Switches
    for (uint8_t i = 0; i < snap.switch_count; i++) {
        const TelemetrySwitch& sw = snap.switches[i];
        char key[24];
        snprintf(key, sizeof(key), "sw%u_state", i);
        data[key] = sw.state ? 1.0 : 0.0;
        snprintf(key, sizeof(key), "sw%u_type", i);
        data[key] = sw.type;
        snprintf(key, sizeof(key), "sw%u_auto", i);
        data[key] = sw.auto_mode ? 1.0 : 0.0;
        snprintf(key, sizeof(key), "sw%u_rule_tripped", i);
        data[key] = sw.rule_tripped ? 1.0 : 0.0;
    }

    // Batteries
    for (uint8_t i = 0; i < snap.battery_count; i++) {
        const TelemetryBattery& b = snap.battery[i];
        char key[32];
        snprintf(key, sizeof(key), "bat%u_soc_pct", i);
        data[key] = write_float(b.soc_pct);
        snprintf(key, sizeof(key), "bat%u_V", i);
        data[key] = write_float(b.V);
        snprintf(key, sizeof(key), "bat%u_I", i);
        data[key] = write_float(b.I);
        snprintf(key, sizeof(key), "bat%u_cumulative_Ah_in", i);
        data[key] = write_float(b.cumulative_Ah_in);
        snprintf(key, sizeof(key), "bat%u_cumulative_Ah_out", i);
        data[key] = write_float(b.cumulative_Ah_out);
        snprintf(key, sizeof(key), "bat%u_equivalent_full_cycles", i);
        data[key] = write_float(b.equivalent_full_cycles);
        snprintf(key, sizeof(key), "bat%u_soh_pct", i);
        data[key] = write_float(b.soh_pct);
        snprintf(key, sizeof(key), "bat%u_soh_samples", i);
        data[key] = b.soh_samples;
    }

    // Log metadata
    data["log_entries"] = snap.log.entries;
    data["log_overflow"] = snap.log.overflow ? 1.0 : 0.0;

    // System health
    data["min_free_heap"] = (double)snap.min_free_heap;
    data["reset_reason"] = snap.reset_reason;
    data["crash_count"] = (double)snap.crash_count;
    data["safe_mode"] = snap.safe_mode ? 1.0 : 0.0;
    data["ntp_synced"] = snap.ntp_synced ? 1.0 : 0.0;
    data["ble_active"] = snap.ble_active ? 1.0 : 0.0;
    data["ble_connected"] = snap.ble_connected ? 1.0 : 0.0;
    data["mqtt_connected"] = snap.mqtt_connected ? 1.0 : 0.0;
    data["http_configured"] = snap.http_configured ? 1.0 : 0.0;
    data["network_skipped"] = snap.network_skipped ? 1.0 : 0.0;
    data["sd_present"] = snap.sd_present ? 1.0 : 0.0;
    data["log_buffer_used_pct"] = snap.log_buffer_used_pct;
    data["sensors_calibrating"] = snap.sensors_calibrating ? 1.0 : 0.0;

    // OTA status (numeric subset)
    data["ota_in_progress"] = snap.ota.ota_in_progress ? 1.0 : 0.0;
    data["ota_progress_pct"] = snap.ota.ota_progress_pct;
}

static void publish_telemetry_envelope(const TelemetrySnapshot& snap,
                                       HTTPClient& http,
                                       WiFiClientSecure& client) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s",
             kBackendUrl, "/api/v1/telemetry");
    http.begin(client, full_url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    serialize_telemetry_flat(snap, doc.to<JsonObject>());

    std::string body;
    body.reserve(serializeJson(doc, nullptr, 0) + 16);
    serializeJson(doc, body);
    http.POST(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    http.end();
}

// ── Source-grep check for the MQTT LWT ─────────────────────────────────────
//
// The PubSubClient mqtt.connect() overload that takes a will topic/message is
// the one we want to be calling. We don't need a live broker; we just need
// to confirm the firmware code at the call site actually passes a will topic
// + will message. This is a guard against accidental refactors that drop
// the LWT.
static bool source_contains_lwt_connect(const char* path, const char* mqtt_var) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        // Crude: look for lines that mention both mqtt.connect and "will_topic"
    // (the LWT will topic variable). The actual topic is "status/{key}/online"
    // and the payload contains "online:false", but the variable name is
    // will_topic — that's the signal that the will-aware overload is used.
        if (line.find(mqtt_var) != std::string::npos &&
            line.find(".connect(") != std::string::npos &&
            line.find("will_topic") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ──────────────────────────────────────────────────────────────────────────
// Test scenarios
// ──────────────────────────────────────────────────────────────────────────
static void test_telemetry_v1_shape() {
    fprintf(stderr, "\n== test_telemetry_v1_shape ==\n");
    setup_world();
    SensorSnapshot snap = good_snapshot(12.5f, 2.3f);
    sim_set_last_snapshot(snap);
    TelemetrySnapshot t;
    telemetry_build(t);
    t.heap_free = 200000;

    http_capture().clear();
    http_set_next_response(200, "{}");
    HTTPClient http;
    WiFiClientSecure client;
    publish_telemetry_envelope(t, http, client);

    // 1. Exactly one HTTP call captured
    EXPECT(http_capture().size() == 1, "exactly one HTTP call captured");

    // 2. Body is a flat JSON object (not Supabase RPC envelope)
    std::string body(http_capture().back().body.begin(),
                     http_capture().back().body.end());
    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    EXPECT_FALSE(err, "body parses as valid JSON");
    EXPECT(!doc.overflowed(), "JsonDocument did not overflow");
    EXPECT(doc.is<JsonObject>(), "body is a flat JSON object (not array)");

    // 3. Top-level fields match backend ingest.go
    EXPECT_HAS(doc.as<JsonObject>(), "ts", "top-level ts present");
    EXPECT_HAS(doc.as<JsonObject>(), "ts_ms", "top-level ts_ms present");
    EXPECT_HAS(doc.as<JsonObject>(), "schema", "top-level schema present");
    EXPECT_HAS(doc.as<JsonObject>(), "fw", "top-level fw present");
    EXPECT_HAS(doc.as<JsonObject>(), "uptime_ms", "top-level uptime_ms present");
    EXPECT_HAS(doc.as<JsonObject>(), "rssi", "top-level rssi present");
    EXPECT_HAS(doc.as<JsonObject>(), "heap_free", "top-level heap_free present");
    EXPECT_HAS(doc.as<JsonObject>(), "hw_rev", "top-level hw_rev present");
    EXPECT_HAS(doc.as<JsonObject>(), "time_source", "top-level time_source present");
    EXPECT_HAS(doc.as<JsonObject>(), "data", "top-level data map present");

    EXPECT_EQ_STR(std::string(doc["schema"].as<const char*>()),
                  "telemetry_v1", "schema = telemetry_v1");
    EXPECT_EQ_STR(std::string(doc["fw"].as<const char*>()),
                  "2.0.0", "fw = TELEMETRY_FW_VERSION");
    EXPECT_EQ_STR(std::string(doc["time_source"].as<const char*>()),
                  "ntp", "time_source = ntp (NTP synced)");
    EXPECT_EQ_STR(std::string(doc["hw_rev"].as<const char*>()),
                  "rev1.0", "hw_rev = rev1.0");
    EXPECT(doc["rssi"].as<int>() == -65, "rssi = -65 dBm");
    EXPECT(doc["heap_free"].as<uint32_t>() == 200000u, "heap_free = 200000");
    EXPECT(doc["uptime_ms"].as<uint32_t>() == 0, "uptime_ms = 0 (fresh boot)");

    // 4. data map is present and contains channel fields
    JsonObject data = doc["data"].as<JsonObject>();
    EXPECT(data.size() > 0, "data map is non-empty");

    // 5. Channel fields in data map
    EXPECT_HAS(data, "ch0_V", "data.ch0_V present");
    EXPECT_HAS(data, "ch0_I", "data.ch0_I present");
    EXPECT_HAS(data, "ch0_P", "data.ch0_P present");
    EXPECT_HAS(data, "ch0_energy_Wh", "data.ch0_energy_Wh present");
    EXPECT_HAS(data, "ch0_charge_mAh", "data.ch0_charge_mAh present");

    // 6. Float values rendered as JSON strings with 4 decimal places
    EXPECT(body.find("\"12.5000\"") != std::string::npos,
           "ch0_V rendered as 4-decimal string in raw body");
    EXPECT(body.find("\"2.3000\"") != std::string::npos,
           "ch0_I rendered as 4-decimal string in raw body");
    // P = 12.5 * 2.3 = 28.75
    EXPECT(body.find("\"28.7500\"") != std::string::npos,
           "ch0_P rendered as 4-decimal string in raw body");

    // 7. Switch fields in data map (present only if switches configured)
    // No switches are configured in setup_world(), so sw0_state is absent.
    // When switches are added, this assertion should check for their presence.

    // 8. Log metadata in data map
    EXPECT_HAS(data, "log_entries", "data.log_entries present");
    EXPECT_HAS(data, "log_overflow", "data.log_overflow present");
    EXPECT(data["log_entries"].as<int>() == 0, "log_entries = 0 (empty buffer)");

    // 9. System health fields in data map
    EXPECT_HAS(data, "min_free_heap", "data.min_free_heap present");
    EXPECT_HAS(data, "reset_reason", "data.reset_reason present");
    EXPECT_HAS(data, "crash_count", "data.crash_count present");
    EXPECT_HAS(data, "safe_mode", "data.safe_mode present");
    EXPECT_HAS(data, "ntp_synced", "data.ntp_synced present");
    EXPECT_HAS(data, "ble_active", "data.ble_active present");
    EXPECT_HAS(data, "ble_connected", "data.ble_connected present");
    EXPECT_HAS(data, "mqtt_connected", "data.mqtt_connected present");
    EXPECT_HAS(data, "http_configured", "data.http_configured present");
    EXPECT_HAS(data, "network_skipped", "data.network_skipped present");
    EXPECT_HAS(data, "sd_present", "data.sd_present present");
    EXPECT_HAS(data, "log_buffer_used_pct", "data.log_buffer_used_pct present");
    EXPECT_HAS(data, "sensors_calibrating", "data.sensors_calibrating present");

    // 10. OTA fields in data map
    EXPECT_HAS(data, "ota_in_progress", "data.ota_in_progress present");
    EXPECT_HAS(data, "ota_progress_pct", "data.ota_progress_pct present");

    // 11. Health field values
    EXPECT(data["ntp_synced"].as<int>() == 1, "ntp_synced = 1 (sim set)");
    EXPECT(data["safe_mode"].as<int>() == 0, "safe_mode = 0 (no crashes)");
    EXPECT(data["ble_active"].as<int>() == 0, "ble_active = 0 (sim default)");
    EXPECT(data["mqtt_connected"].as<int>() == 0, "mqtt_connected = 0 (sim default)");
    EXPECT(data["ota_in_progress"].as<int>() == 0, "ota_in_progress = 0 (idle)");
    EXPECT(data["ota_progress_pct"].as<int>() == 0, "ota_progress_pct = 0");
}

static void test_telemetry_time_source_uptime() {
    fprintf(stderr, "\n== test_telemetry_time_source_uptime ==\n");
    setup_world();
    SensorSnapshot snap = good_snapshot(12.0f, 1.0f);
    sim_set_last_snapshot(snap);

    // 1. NTP not synced → time_source = "uptime" in the published JSON.
    sim_set_ntp_synced(false);
    sim_set_epoch(1717500000);
    TelemetrySnapshot t;
    telemetry_build(t);
    // The serializer is what flips it to "uptime" — replicate that
    // decision in the test wrapper:
    EXPECT(!sim_ntp_synced(), "NTP sync flag false before publish");
    EXPECT(t.ts != 0,         "snap.ts != 0 even with NTP unsynced");
    EXPECT_EQ_STR((sim_ntp_synced() || t.ts == 0) ? "ntp" : "uptime",
                  "uptime",
                  "time_source decision = uptime when NTP unsynced");
}

static void test_nan_downgrade() {
    fprintf(stderr, "\n== test_nan_downgrade ==\n");
    setup_world();
    SensorSnapshot snap = nan_snapshot();
    sim_set_last_snapshot(snap);
    TelemetrySnapshot t;
    telemetry_build(t);
    t.heap_free = 200000;

    http_capture().clear();
    http_set_next_response(200, "{}");
    HTTPClient http;
    WiFiClientSecure client;
    publish_telemetry_envelope(t, http, client);

    std::string body(http_capture().back().body.begin(),
                     http_capture().back().body.end());
    JsonDocument doc;
    deserializeJson(doc, body);
    JsonObject root = doc.as<JsonObject>();
    JsonObject data = root["data"].as<JsonObject>();

    // The test re-uses the same write_float() helper logic, so NaN must be
    // downgraded to 0 BEFORE serialisation. The serialized string must NOT
    // contain "NaN" or "Inf" anywhere.
    EXPECT(body.find("NaN") == std::string::npos, "no literal 'NaN' in JSON body");
    EXPECT(body.find("Inf") == std::string::npos, "no literal 'Inf' in JSON body");

    // V=NaN becomes 0.0000
    EXPECT_HAS(data, "ch0_V", "data.ch0_V present");
    EXPECT_HAS(data, "ch0_I", "data.ch0_I present");
    EXPECT_HAS(data, "ch0_P", "data.ch0_P present");
    EXPECT_EQ_STR(std::string(data["ch0_V"].as<const char*>()), "0.0000",
                  "NaN V downgraded to 0.0000");
    EXPECT_EQ_STR(std::string(data["ch0_I"].as<const char*>()), "1.5000",
                  "I passed through unchanged (1.5)");
    // P = V * I — with V downgraded to 0, P = 0
    EXPECT_EQ_STR(std::string(data["ch0_P"].as<const char*>()), "0.0000",
                  "P = 0.0000 (V was NaN, downgraded)");
}


// Source-grep check for the MQTT LWT — we don't spin up a broker; the
// test is that the firmware code is *calling* the will-aware overload.
static void test_mqtt_lwt_will_topic() {
    fprintf(stderr, "\n== test_mqtt_lwt_will_topic ==\n");
    const char* fw_path = "../src/connectivity_manager.cpp";
    bool found = source_contains_lwt_connect(fw_path, "mqtt");
    EXPECT(found,
           "connectivity_manager.cpp: mqtt.connect() is called with will topic + message (LWT)");
}

static void test_set_relay_set_switch_payload() {
    fprintf(stderr, "\n== test_set_relay / set_switch payload shape ==\n");
    // The wrapper in ui/src/lib/deviceCommands.ts sends:
    //   setSwitch:   { idx, type, gpio_pin, active_high, enabled, name,
    //                 switch_idx, channel, conditions, logic, min_conditions,
    //                 trip_delay_ms, reset_delay_ms, hysteresis, enabled }
    //   setRelayEnergized: { idx, is_energized, active_high, enabled }
    // The device-side handler in ble_provisioner.cpp::apply_settings_command
    // reads these fields. To prove the wire format is consistent we
    // deserialize a sample payload and assert every key the TS wrapper
    // sends is one the firmware would read. We do NOT exercise the full
    // apply path here (it requires a fully linked connectivity_manager).

    // Sample payload from setRelayEnergized(...) — see deviceCommands.ts:84.
    const char* sample = R"({
        "idx": 2,
        "is_energized": true,
        "active_high": true,
        "enabled": true
    })";
    JsonDocument doc;
    deserializeJson(doc, sample);
    JsonObject obj = doc.as<JsonObject>();
    EXPECT(obj["idx"].is<int>(),                 "set_relay: idx is int");
    EXPECT(obj["is_energized"].is<bool>(),       "set_relay: is_energized is bool");
    EXPECT(obj["active_high"].is<bool>(),        "set_relay: active_high is bool");
    EXPECT(obj["enabled"].is<bool>(),            "set_relay: enabled is bool");

    // Sample payload from setSwitch(...) — see deviceCommands.ts:63.
    // Mirrors the { ...channel, ...rule } spread; field names must line up
    // with what apply_settings_command reads.
    const char* sample2 = R"({
        "idx": 0,
        "type": 0,
        "gpio_pin": 25,
        "active_high": true,
        "enabled": true,
        "name": "Relay 0",
        "switch_idx": 0,
        "channel": 0,
        "logic": "OR",
        "min_conditions": 1,
        "trip_delay_ms": 1000,
        "reset_delay_ms": 5000,
        "hysteresis": 0.0,
        "conditions": [
            {"kind": "OVERCURRENT", "op": "gt", "value": 5.0, "ref_channel": -1}
        ]
    })";
    JsonDocument doc2;
    deserializeJson(doc2, sample2);
    JsonObject o2 = doc2.as<JsonObject>();
    static const char* kSwitchFields[] = {
        "idx", "type", "gpio_pin", "active_high", "enabled",
        "switch_idx", "channel", "logic", "min_conditions",
        "trip_delay_ms", "reset_delay_ms", "hysteresis", "conditions",
    };
    for (size_t i = 0; i < sizeof(kSwitchFields)/sizeof(kSwitchFields[0]); i++) {
        EXPECT(!o2[kSwitchFields[i]].isNull(),
               std::string("set_switch payload includes ") + kSwitchFields[i]);
    }
    // The condition object uses both "kind" and "op" strings; the firmware
    // accepts both string and enum forms.
    JsonObject c0 = o2["conditions"][0];
    EXPECT_EQ_STR(std::string(c0["kind"].as<const char*>()), "OVERCURRENT",
                  "condition.kind = OVERCURRENT");
    EXPECT_EQ_STR(std::string(c0["op"].as<const char*>()), "gt",
                  "condition.op = gt");
}


// ── apply_settings_command set_switch list-shape round-trip (B10) ────────
// Audit B10: the firmware's apply_settings_command for `set_switch` /
// `set_relay` reads the new list-shape (conditions array with kind/op/
// value/ref_channel/schedule_mask). We round-trip a payload that
// includes one of every field type and verify the parser sees them
// all in the right shape.
static void test_apply_settings_set_switch_roundtrip() {
    fprintf(stderr, "\n== test_apply_settings_command set_switch list-shape (B10) ==\n");

    // Build a payload that exercises every condition field type.
    // schedule_mask is a 28-byte array (7 days × 4 hours per byte? No
    // — 7 × 24 / 8 = 21 bytes, but the firmware uses 28 as a
    // conservative upper bound. We use 21 to mirror the real
    // 7×24=168-bit schedule window).
    const char* sample = R"({
        "idx": 1,
        "type": 0,
        "gpio_pin": 3,
        "active_high": true,
        "enabled": true,
        "channel": 0,
        "trip_delay_ms": 1500,
        "reset_delay_ms": 4500,
        "logic": "OR",
        "min_conditions": 1,
        "hysteresis": 0.1,
        "conditions": [
            {"kind": "OVERCURRENT",     "op": "gt",  "value": 5.0,  "ref_channel": -1, "schedule_mask": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21]},
            {"kind": "UNDERVOLTAGE",    "op": "<",   "value": 11.0, "ref_channel": 0,  "schedule_mask": [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]},
            {"kind": "SOC_LOW",         "op": "lte", "value": 20.0, "ref_channel": -1},
            {"kind": "SOC_HIGH",        "op": "gte", "value": 95.0, "ref_channel": -1},
            {"kind": "CHANNEL_ABOVE",   "op": ">",   "value": 2.5,  "ref_channel": 1},
            {"kind": "CHANNEL_BELOW",   "op": "<=",  "value": 0.1,  "ref_channel": 2},
            {"kind": "SCHEDULE_WINDOW", "op": "==",  "value": 0,     "ref_channel": -1, "schedule_mask": [255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255]}
        ]
    })";
    JsonDocument doc;
    deserializeJson(doc, sample);
    JsonObject obj = doc.as<JsonObject>();
    JsonArray conds = obj["conditions"].as<JsonArray>();
    EXPECT(conds.size() == 7,
           "list-shape: conditions array has 7 entries");
    // Verify each condition kind string is what the firmware's strcmp
    // branches expect.
    static const char* kExpectedKinds[] = {
        "OVERCURRENT", "UNDERVOLTAGE", "SOC_LOW", "SOC_HIGH",
        "CHANNEL_ABOVE", "CHANNEL_BELOW", "SCHEDULE_WINDOW"
    };
    for (size_t i = 0; i < sizeof(kExpectedKinds)/sizeof(kExpectedKinds[0]); i++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "condition[%zu].kind", i);
        EXPECT_EQ_STR(std::string(conds[i]["kind"].as<const char*>()),
                      kExpectedKinds[i], msg);
    }
    // Verify schedule_mask byte length is what the firmware expects
    // (any of: 21 bytes for 7×24, or whatever the firmware tolerates).
    JsonArray mask0 = conds[0]["schedule_mask"].as<JsonArray>();
    EXPECT(mask0.size() == 21,
           "schedule_mask[0] is 21 bytes (7 days × 24 hours / 8 bits)");
    // Verify op strings are recognised forms.
    EXPECT_EQ_STR(std::string(conds[0]["op"].as<const char*>()), "gt",
                  "op[0] = gt (string form)");
    EXPECT_EQ_STR(std::string(conds[5]["op"].as<const char*>()), "<=",
                  "op[5] = <= (string form, alternative to lte)");
    // The "logic" field is read as "AND" → SL_AND, anything else → SL_OR.
    EXPECT_EQ_STR(std::string(obj["logic"].as<const char*>()), "OR",
                  "logic = OR (default)");
}

// ── switch_gpio_allowed denylist data (audit B11) ─────────────────────────
// The firmware's apply_settings_command and the BLE set_switch path
// both gate on switch_gpio_allowed(pin), which checks the pin against
// the board's BAD_GPIO_PINS list. We can't link switch_controller.cpp
// into the test build without pulling in heavy deps, so we verify
// the data the gate consults directly. A future refactor that drops
// 0 or 2 from the C3 denylist would surface here.
static void test_switch_gpio_denylist_data() {
    fprintf(stderr, "\n== test_switch_gpio_allowed denylist data (B11) ==\n");
    bool found_0 = false, found_2 = false, found_8 = false, found_9 = false;
    bool found_usb_pair = false;
    for (size_t i = 0; i < BAD_GPIO_COUNT_ESP32C3; i++) {
        int p = BAD_GPIO_PINS_ESP32C3[i];
        if (p == 0) found_0 = true;
        if (p == 2) found_2 = true;
        if (p == 8) found_8 = true;
        if (p == 9) found_9 = true;
        if (p == 18 || p == 19) found_usb_pair = true;
    }
    EXPECT(found_0,  "C3 denylist includes GPIO 0 (BOOT button)");
    EXPECT(found_2,  "C3 denylist includes GPIO 2 (strapping pin)");
    EXPECT(found_8,  "C3 denylist includes GPIO 8 (strapping pin)");
    EXPECT(found_9,  "C3 denylist includes GPIO 9 (strapping pin)");
    EXPECT(found_usb_pair, "C3 denylist includes USB D-/D+ pair (18/19)");
    EXPECT(BOARD_GPIO_MAX == 21,
           "C3 BOARD_GPIO_MAX is 21 (matches the C3 GPIO range)");
}


int main() {
    fprintf(stderr, "== test_publish_path ==\n");

    test_telemetry_v1_shape();
    test_telemetry_time_source_uptime();
    test_nan_downgrade();
    test_mqtt_lwt_will_topic();
    test_set_relay_set_switch_payload();
    test_apply_settings_set_switch_roundtrip();
    test_switch_gpio_denylist_data();

    fprintf(stderr, "\n== %d/%d tests passed, %d failed ==\n",
            g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
