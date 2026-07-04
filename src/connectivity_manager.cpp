#include "connectivity_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "log_serial.h"
#include "settings_manager.h"
#include "data_logger.h"
#include "ble_provisioner.h"
#include "battery_profile.h"
#include "coulomb_counter.h"
#include "energy_counter.h"
#include "sensor_manager.h"
#include "switch_controller.h"
#include "telemetry.h"
#include <WiFi.h>
#include <esp_system.h>
#include <WiFiClientSecure.h>
// Bumped to 2048 so JSON telemetry publishes with metadata + 4 channel rows
// (≈ 1.4 KB serialized) fit without truncation. The PubSubClient library
// reads this #define at include time, hence the guard. (Mirrored in
// config.h so the value lives with the other compile-time settings.)
#include <PubSubClient.h>
#include <ArduinoJson.h>
// #include <BlynkSimpleEsp32.h> // Blynk disabled
// Blynk disabled — uncomment above and set BLYNK_AUTH_TOKEN to enable
#include <HTTPClient.h>
#include <time.h>
#include <stdlib.h>     // setenv
#include <math.h>       // isfinite
#include <esp_sntp.h>

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static char ip_str[16] = "0.0.0.0";

static bool skip_network = false;

static time_t epoch_time = 0;

static bool is_valid_uuid(const char* s) {
    if (!s || strlen(s) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else {
            if (!isxdigit((unsigned char)s[i])) return false;
        }
    }
    return true;
}

// Persistent HTTPS clients for Supabase
// g_telemetry_http: connection reuse for 5s telemetry POSTs (high frequency)
// g_supa_http: fresh connection per request for low-frequency calls (relays, settings)
static WiFiClientSecure g_telemetry_client;
static HTTPClient        g_telemetry_http;
static bool              g_telemetry_http_ready = false;
static bool              g_telemetry_error = false;  // force reset after POST failure
static WiFiClientSecure  g_supa_client;
static HTTPClient        g_supa_http;
static bool              g_supa_http_ready = false;
static unsigned long g_defer_cooldown = 0;
static uint16_t g_deferred_requests = 0;
static uint8_t  g_deferred_relay_idx = 0;
static bool     g_deferred_relay_state = false;

// Heap-guard thresholds. All Supabase/MQTT publish paths bail if free heap
// drops below MIN_FREE_HEAP_FOR_PUBLISH; publish_switch_state / sync_*
// operations (low frequency) can use a slightly lower threshold. Centralised
// so a single tuning change covers every site.
static const uint32_t MIN_FREE_HEAP_FOR_PUBLISH   = 13000;  // full HTTP POST + JSON serialize
static const uint32_t MIN_FREE_HEAP_FOR_LOWFREQ   = 8192;   // sync_device_channels / publish_log_batch_supabase
static const uint32_t MIN_FREE_HEAP_FOR_SYNC_OPS  = 4096;   // sync_calibration / sync_ble_pin / calibration_status
static const uint32_t MIN_FREE_HEAP_FOR_SWITCH    = 3072;   // publish_switch_state (small payload)

// sync_device_channels_to_supabase is static and not in header — forward declare
static void sync_device_channels_to_supabase();
static void connect_mqtt();

static void supabase_http_reset() {
    if (g_supa_http_ready) {
        g_supa_http.end();
        g_supa_http_ready = false;
    }
    g_supa_client.stop();
    vTaskDelay(pdMS_TO_TICKS(10));  // let mbedTLS flush before next begin
}

static void drain_response() {
    if (!g_supa_http_ready) return;
    Stream& stream = g_supa_http.getStream();
    unsigned long t0 = millis();
    while (stream.available() && millis() - t0 < 500) {
        stream.read();
    }
}

static void telemetry_http_reset() {
    if (g_telemetry_http_ready) {
        g_telemetry_http.end();
        g_telemetry_client.stop();
        g_telemetry_http_ready = false;
    }
}

static void drain_telemetry_response() {
    if (!g_telemetry_http_ready) return;
    Stream& stream = g_telemetry_http.getStream();
    unsigned long t0 = millis();
    while (stream.available() && millis() - t0 < 500) {
        stream.read();
    }
}

static bool telemetry_http_prepare(const char* full_url, const char* anon_key) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PUBLISH) return false;
    if (!g_telemetry_http_ready) {
        g_telemetry_client.setInsecure();
        g_telemetry_client.setHandshakeTimeout(30);
        g_telemetry_http.setReuse(true);  // connection reuse for high-frequency telemetry
        if (!g_telemetry_http.begin(g_telemetry_client, full_url)) {
            g_telemetry_http_ready = false;
            return false;
        }
        g_telemetry_http.addHeader("Content-Type", "application/json");
        g_telemetry_http.addHeader("Content-Profile", TELEMETRY_PROFILE_STRING);
        g_telemetry_http.addHeader("apikey", anon_key);
        char auth_hdr[384];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", anon_key);
        g_telemetry_http.addHeader("Authorization", auth_hdr);
        g_telemetry_http_ready = true;
    } else {
        // Force reset when connection is dead — prevents esp-aes allocate failure
        // on stale TLS sessions after WiFi reconnect or server close.
        if (!g_telemetry_client.connected() || g_telemetry_error) {
            telemetry_http_reset();
            g_telemetry_error = false;
            return telemetry_http_prepare(full_url, anon_key);
        }
        if (!g_telemetry_http.begin(g_telemetry_client, full_url)) {
            telemetry_http_reset();
            return false;
        }
    }
    return true;
}

static bool supabase_http_prepare(const char* full_url, const char* anon_key) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_LOWFREQ) return false;
    if (g_supa_http_ready) {
        // Always tear down and rebuild — setReuse(false) means no pooling benefit,
        // and WiFiClientSecure::connected() only checks TCP, not TLS session health.
        // Keeping a dead TLS session alive causes rc=-1 on every subsequent POST.
        supabase_http_reset();
    }

    // Reuse the static g_supa_client — first call configures insecure + handshake
    // timeout once. Subsequent calls skip reconfiguration since the underlying
    // mbedTLS context is reset() rather than destroyed.
    static bool g_supa_client_configured = false;
    if (!g_supa_client_configured) {
        g_supa_client.setInsecure();
        g_supa_client.setHandshakeTimeout(30);
        g_supa_client_configured = true;
    }
    g_supa_http.setReuse(false);
    if (!g_supa_http.begin(g_supa_client, full_url)) {
        LOG_PRINT("[SUPA_HTTP] begin failed: %s\n", full_url);
        supabase_http_reset();
        return false;
    }

    g_supa_http.addHeader("Content-Type", "application/json");
    g_supa_http.addHeader("apikey", anon_key);
    char auth_hdr[384];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", anon_key);
    g_supa_http.addHeader("Authorization", auth_hdr);
    g_supa_http_ready = true;
    return true;
}

static int telemetry_post(const char* url_path, const char* payload, size_t len,
    const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!telemetry_http_prepare(full_url, anon_key)) return -1;
    // LOG_PRINT("[HTTP] POST %d bytes (heap=%u)\n", len, ESP.getFreeHeap());
    // LOG_PRINTLN(payload);  // verbose — disabled
    int rc = g_telemetry_http.POST((uint8_t*)payload, len);
    if (rc < 0) {
        drain_telemetry_response();
        telemetry_http_reset();
        g_telemetry_error = true;  // force fresh connection on next call
    } else {
        drain_telemetry_response();
    }
    return rc;
}

static int supabase_post(const char* url_path, const char* payload, size_t len,
    const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!supabase_http_prepare(full_url, anon_key)) return -1;
    int rc = g_supa_http.POST((uint8_t*)payload, len);
    if (rc < 0) {
        supabase_http_reset();
        delay(50);
        if (!supabase_http_prepare(full_url, anon_key)) return -1;
        rc = g_supa_http.POST((uint8_t*)payload, len);
    }
    drain_response();
    supabase_http_reset();
    return rc;
}

static int supabase_patch(const char* url_path, const char* payload, size_t len,
    const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!supabase_http_prepare(full_url, anon_key)) return -1;
    int rc = g_supa_http.sendRequest("PATCH", (uint8_t*)payload, len);
    if (rc < 0) {
        supabase_http_reset();
        delay(50);
        if (!supabase_http_prepare(full_url, anon_key)) return -1;
        rc = g_supa_http.sendRequest("PATCH", (uint8_t*)payload, len);
    }
    drain_response();
    supabase_http_reset();
    return rc;
}

static int supabase_get(const char* url_path, const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!supabase_http_prepare(full_url, anon_key)) return -1;
    int rc = g_supa_http.GET();
    if (rc < 0) supabase_http_reset();
    return rc;
}

// src: 0=none, 1=ina3221_volt(0x42), 2=ina3221_curr(0x40), 3=ina226, 4=ads1115
// These legacy source IDs are mapped onto the new flat logical-channel view.
float get_sensor_voltage(uint8_t src, uint8_t idx, const SensorSnapshot& data) {
    (void)data;
    if (src == 1 || src == 2) return get_channel_voltage(idx < 4 ? idx : 0);   // logical channel voltage
    if (src == 3) return get_channel_voltage(3);                                // INA226 logical channel
    if (src == 4) return 0.0f;                                                  // reserved
    return 0.0f;
}
float get_sensor_current(uint8_t src, uint8_t idx, const SensorSnapshot& data) {
    (void)data;
    if (src == 1 || src == 2) return get_channel_current(idx < 4 ? idx : 0);   // logical channel current
    if (src == 3) return get_channel_current(3);                                // INA226 logical channel
    return 0.0f;
}
float get_sensor_power(uint8_t src, uint8_t idx, const SensorSnapshot& data) {
    (void)data;
    if (src == 3) return get_channel_power(3);                                  // INA226 logical channel
    return 0.0f;
}

static bool sntp_started = false;

static void start_sntp() {
    if (sntp_started) return;
    // Pin C runtime TZ to UTC so getLocalTime/mktime round-trip is a no-op.
    // Without this, newlib's default TZ (EST5EDT on ESP32 Arduino) leaks
    // into the SNTP→epoch conversion and drifts the clock.
    setenv("TZ", "UTC0", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    sntp_started = true;
}

// NTP / wall-clock sync — bounded wait so a flaky network can't stall the
// network task. Returns one of:
//   SYNC_OK      — time() returned a sane post-2023 epoch
//   SYNC_TIMEOUT — start_sntp() called but time() still < 1700000000 after
//                  NTP_TIMEOUT_MS milliseconds
//   SYNC_NO_WIFI — caller is in offline mode (skip_network) and there's no
//                  point trying
// On TIMEOUT we set g_ntp_synced=false so the data logger can downgrade
// timestamps to "uptime" mode and consumers can stop trusting epoch math.
enum SyncResult { SYNC_OK = 0, SYNC_TIMEOUT, SYNC_NO_WIFI };
static bool g_ntp_synced = false;
static bool g_ntp_timeout_warned = false;
static const uint32_t NTP_TIMEOUT_MS = 5000;

bool ntp_is_synced() { return g_ntp_synced; }

static SyncResult sync_time() {
    if (skip_network) return SYNC_NO_WIFI;
    start_sntp();
    // Poll time() for up to NTP_TIMEOUT_MS rather than blocking forever.
    // SNTP runs in the background; we just need to know if it has produced
    // a sensible answer within the budget.
    uint32_t deadline = millis() + NTP_TIMEOUT_MS;
    time_t t = 0;
    while (millis() < deadline) {
        t = time(nullptr);
        if (t > 1700000000) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (t < 1700000000) {
        if (!g_ntp_timeout_warned) {
            LOG_PRINT("[NTP] sync timeout after %u ms — timestamps will use uptime\n",
                          (unsigned)NTP_TIMEOUT_MS);
            g_ntp_timeout_warned = true;
        }
        // Mark the data logger as untrusted so any future log_sample() knows
        // to flag the uptime-based timestamp.
        log_epoch_valid_set(false);
        g_ntp_synced = false;
        return SYNC_TIMEOUT;
    }
    epoch_time = t;
    log_set_epoch(t);
    log_epoch_valid_set(true);  // trusted post-2023 epoch
    g_ntp_synced = true;
    // Reset the warning latch so the next timeout (e.g. after a WiFi
    // reconnect) prints a fresh message instead of staying silent.
    g_ntp_timeout_warned = false;
    struct tm ti = {};
    gmtime_r(&t, &ti);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &ti);
    LOG_PRINTLN("NTP time: "); LOG_PRINTLN(buf);
    return SYNC_OK;
}

bool try_sync_epoch_time() {
    // Re-sync hourly so drift / failed first-sync recovers automatically.
    static unsigned long last_sync_ms = 0;
    if (epoch_time > 0 && millis() - last_sync_ms < 3600UL * 1000) return true;
    SyncResult r = sync_time();
    if (r == SYNC_OK) {
        last_sync_ms = millis();
        return true;
    }
    return epoch_time > 0;
}

// ============================================================================
// WiFi init as a non-blocking state machine.
//
// Earlier code blocked up to 10 s in connect_wifi() and another 3 s in
// init_connectivity(), stalling the entire networkTask. Other tasks (sensor,
// UI) would also be starved because networkTask is the only thing on Core 0
// driving the heap-heavy Supabase path; the Arduino loop on Core 1 stayed
// alive but couldn't do much.
//
// The state machine:
//   INIT          — pick credentials, call WiFi.begin() once
//   CONNECTING    — wait up to 5 s for WL_CONNECTED
//   CONNECTED     — disable BLE, configure MQTT, run sync_time() etc.
//   RETRY_BACKOFF — last attempt failed; wait 30 s, then go to CONNECTING
// State persists in static locals inside this translation unit. The network
// task calls wifi_state_tick() once per 10 ms loop tick.
// ============================================================================
enum WifiState { WST_INIT, WST_CONNECTING, WST_CONNECTED, WST_RETRY_BACKOFF };
static WifiState s_wifi_state = WST_INIT;
static uint32_t s_wifi_state_entered_ms = 0;
static uint32_t s_wifi_connect_deadline = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 5000;
static const uint32_t WIFI_RETRY_BACKOFF_MS = 30000;

static void wifi_state_enter(WifiState s) {
    s_wifi_state = s;
    s_wifi_state_entered_ms = millis();
}

static void wifi_state_tick() {
    switch (s_wifi_state) {
        case WST_INIT: {
            // Load credentials and start WiFi.begin(). Cached so we don't
            // hit NVS again on the retry path.
            char ssid[64] = "", pass[64] = "";
            if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
                WiFi.begin(ssid, pass);
            } else if (strlen(WIFI_SSID) > 5 && strcmp(WIFI_SSID, "YOUR_SSID") != 0) {
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            } else {
                LOG_PRINTLN("WiFi: no credentials — offline mode");
                skip_network = true;
                wifi_state_enter(WST_RETRY_BACKOFF);
                return;
            }
            WiFi.setAutoReconnect(true);
            WiFi.setTxPower(WIFI_POWER_8_5dBm);
            s_wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
            LOG_PRINTLN("[WiFi] connecting");
            wifi_state_enter(WST_CONNECTING);
            return;
        }
        case WST_CONNECTING: {
            if (WiFi.status() == WL_CONNECTED) {
                LOG_PRINTLN(" — connected");
                IPAddress ip = WiFi.localIP();
                snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                // Disable BLE to free ~50KB heap for TLS operations.
                LOG_PRINTLN("[BLE] disabling BLE stack to free heap for TLS");
                deinit_ble_provisioner();
                // Bounded NTP sync; may fail if no internet, that's fine.
                SyncResult r = sync_time();
                if (r == SYNC_OK) {
                    sync_calibration_to_supabase();
                }
                // Configure MQTT broker (we don't block on connect — connect_mqtt
                // is rate-limited and runs in loop_connectivity).
                char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
                if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
                    mqtt.setServer(mqtt_broker, mqtt_port);
                    connect_mqtt();
                } else {
                    LOG_PRINTLN("MQTT: not configured (skip)");
                }
                wifi_state_enter(WST_CONNECTED);
                return;
            }
            if (millis() >= s_wifi_connect_deadline) {
                LOG_PRINTLN("\n[WiFi] connect timeout — backing off");
                skip_network = true;  // mirror old behavior; loop_connectivity
                                       // will retry from WST_RETRY_BACKOFF
                wifi_state_enter(WST_RETRY_BACKOFF);
                return;
            }
            // Heartbeat dot so a slow user can see progress.
            static uint32_t last_dot_ms = 0;
            if (millis() - last_dot_ms > 500) {
                last_dot_ms = millis();
                LOG_PRINTLN(".");
            }
            return;
        }
        case WST_CONNECTED:
            // Steady state. Disconnect detection happens in loop_connectivity.
            return;
        case WST_RETRY_BACKOFF:
            if (millis() - s_wifi_state_entered_ms >= WIFI_RETRY_BACKOFF_MS) {
                LOG_PRINTLN("[WiFi] retrying after backoff");
                skip_network = false;
                wifi_state_enter(WST_INIT);
            }
            return;
    }
}

static void connect_mqtt() {
    if (skip_network) return;
    static uint32_t last_mqtt_retry = -30000UL; // underflow so first call passes
    if (millis() - last_mqtt_retry < 30000) return; // rate limit: 1 attempt per 30s
    last_mqtt_retry = millis();
    // Last-will-and-testament: when the broker sees this client drop, it
    // publishes "offline" to <topic>/status. We override with "online" right
    // after a successful connect so subscribers see the actual state.
    // PubSubClient exposes LWT via the connect() overloads, not a setWill().
    const char* will_topic = MQTT_TOPIC "/status";
    bool connected = mqtt.connect("power-monitor-esp32", will_topic, 1, true, "offline");
    if (connected) {
        LOG_PRINTLN("MQTT connected");
        // Override LWT with our actual liveness state.
        mqtt.publish(will_topic, "online", true);
    } else {
        LOG_PRINT("MQTT fail rc=%d\n", mqtt.state());
    }
}

const char* get_local_ip_str() { return ip_str; }
time_t get_epoch_time() { return epoch_time; }

static void print_http_error(HTTPClient& http, int rc) {
    LOG_PRINT("HTTP error %d (heap=%u / largest=%u)\n",
        rc, ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (rc < 0) return; // no body on connection-level failure
    Stream& stream = http.getStream();
    char body[512];
    int n = 0;
    unsigned long t0 = millis();
    while (stream.available() && n < (int)sizeof(body) - 1 && millis() - t0 < 300) {
        int c = stream.read();
        if (c >= 0) body[n++] = (char)c;
    }
    body[n] = '\0';
    if (n > 0) {
        LOG_PRINTLN("Response body: ");
        LOG_PRINTLN(body);
    }
    // drain any remaining bytes so they don't leak into the next request on a reused connection
    t0 = millis();
    while (stream.available() && millis() - t0 < 500) {
        stream.read();
    }
}

void publish_data_http(const SensorSnapshot& data, const char* json_buffer, size_t json_len) {
    (void)data;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_SYNC_OPS) return;
    if (!settings_load_http_enabled()) return;
    char url[128], token[64];
    if (!settings_load_http_endpoint(url, token, sizeof(url))) return;
    // LOG_PRINT("[HTTP] posting %d bytes to %s\n", json_len, url);
    // LOG_PRINT("[JSON] %.*s\n", json_len < 256 ? json_len : 256, json_buffer);
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    if (strlen(token) > 0) http.addHeader("Authorization", token);
    int rc = http.POST((uint8_t*)json_buffer, json_len);
    if (rc != 200 && rc != 202) {
        print_http_error(http, rc);
    }
    http.end();
}

void init_connectivity() {
    // The actual WiFi bring-up now happens inside the network task's
    // wifi_state_tick() — see wifi_state_enter(WST_INIT) below. This keeps
    // the Arduino loop() / networkTask creation path non-blocking and lets
    // sensor + UI tasks run while WiFi is still associating.
    // We do start BLE advertising here so the device is discoverable for
    // provisioning during the WiFi connect window.
    LOG_PRINTLN("[NET] init_connectivity: WiFi will start in networkTask");
    s_wifi_state = WST_INIT;
    s_wifi_state_entered_ms = millis();
    skip_network = false;
    g_ntp_synced = false;
}

void loop_connectivity() {
    // Drive the WiFi state machine first so the rest of the loop sees a
    // consistent connection state. Each tick is non-blocking: it only
    // advances state when a deadline expires, so most calls are O(1).
    wifi_state_tick();

    static bool wifi_was_connected = false;
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);

    // WiFi came back up after a previous failure — re-enable network.
    if (skip_network && wifi_connected && s_wifi_state == WST_CONNECTED) {
        skip_network = false;
        IPAddress ip = WiFi.localIP();
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        LOG_PRINTLN("[WiFi] connection restored — network re-enabled");
        telemetry_http_reset();  // kill stale TLS session after WiFi reconnect
    }

    // WiFi dropped — restart BLE advertising so device can be re-provisioned.
    if (wifi_was_connected && !wifi_connected) {
        LOG_PRINTLN("[WiFi] disconnected — restarting BLE advertising");
        start_ble_advertising();
        telemetry_http_reset();  // kill TLS session on disconnect to prevent leak
    }
    wifi_was_connected = wifi_connected;

    if (skip_network || !wifi_connected) return;

    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        if (!mqtt.connected()) connect_mqtt();
        mqtt.loop();
    }

    // Process deferred Supabase writes one per loop tick (rate-limited)
    if (g_deferred_requests == 0) return;
    if (millis() - g_defer_cooldown < 2000) return;  // space deferred calls 2s apart
    g_defer_cooldown = millis();

    if (g_deferred_requests & 1) {
        g_deferred_requests &= ~1u;
        sync_device_channels_to_supabase();
        g_defer_cooldown = millis();  // give extra time after full row sync
        return;
    }
    if (g_deferred_requests & 2) {
        g_deferred_requests &= ~2u;
        sync_ble_pin_to_supabase();
        return;
    }
    if (g_deferred_requests & 4) {
        g_deferred_requests &= ~4u;
        publish_switch_state(g_deferred_relay_idx, g_deferred_relay_state);
        return;
    }
    if (g_deferred_requests & 8) {
        g_deferred_requests &= ~8u;
        sync_calibration_to_supabase();
        return;
    }
}

static void base64_encode(const uint8_t* in, size_t in_len, char* out) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < in_len) {
        uint32_t octet_a = i < in_len ? in[i++] : 0;
        uint32_t octet_b = i < in_len ? in[i++] : 0;
        uint32_t octet_c = i < in_len ? in[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        out[j++] = b64[(triple >> 3 * 6) & 0x3F];
        out[j++] = b64[(triple >> 2 * 6) & 0x3F];
        out[j++] = b64[(triple >> 1 * 6) & 0x3F];
        out[j++] = b64[(triple >> 0 * 6) & 0x3F];
    }
    size_t pad = (3 - (in_len % 3)) % 3;
    for (size_t k = 0; k < pad; k++) out[j - 1 - k] = '=';
    out[j] = '\0';
}

bool is_cloud_connected() {
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (!settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) return false;
    return mqtt.connected();
}

void publish_log_batch() {
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (!settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) return;
    if (!mqtt.connected()) return;
    uint8_t batch[512];
    char encoded[700];

    // Drain RAM buffer first
    size_t batch_len = log_pop_batch(batch, sizeof(batch));
    while (batch_len > 0) {
        base64_encode(batch, batch_len, encoded);
        if (!mqtt.publish(MQTT_LOG_TOPIC, encoded)) {
#if CORE_DEBUG_LEVEL >= 3
            LOG_PRINTLN("[MQTT] log publish() returned false — broker dropped or buffer full");
#endif
            break;  // stop draining — broker connection is likely dead
        }
        batch_len = log_pop_batch(batch, sizeof(batch));
    }

    // Then drain LittleFS overflow file
    if (log_open_overflow_for_read()) {
        while (true) {
            size_t n = log_read_overflow_chunk(batch, sizeof(batch));
            if (n == 0) break;
            base64_encode(batch, n, encoded);
            if (!mqtt.publish(MQTT_LOG_TOPIC, encoded)) {
#if CORE_DEBUG_LEVEL >= 3
                LOG_PRINTLN("[MQTT] log publish() returned false during overflow drain");
#endif
                break;
            }
        }
        log_close_overflow();
    }
}

// =============================================================================
// Telemetry serialization — single source of truth for the publish payload
// =============================================================================
// All transports (MQTT, Supabase, HTTP, BLE sensor notify) serialize the same
// TelemetrySnapshot struct. The shape is versioned by TELEMETRY_SCHEMA_VERSION
// in telemetry.h; the server branches on the "schema" field or the
// Content-Profile header (telemetry_v1) to pick a parser.
//
// Floats are rounded to 4 decimal places to keep the payload sane — the
// physical sensors don't need more, and the BLE MTU can't swallow a 6-byte
// mantissa per field.
static constexpr int  FLOAT_DECIMALS = 4;
static constexpr size_t TELEMETRY_BUF_BYTES = 4096;
// Compile-time guard: buffer must be enough for full saturation. The struct
// is bounded above by static_assert in telemetry.h, so 4 KB is well over 2x
// the worst-case JSON output.
static_assert(TELEMETRY_BUF_BYTES >= 2048,
              "Telemetry buffer < 2 KB; raise TELEMETRY_BUF_BYTES or shrink struct");

static void write_float(JsonObject obj, const char* key, float v) {
    // NaN/Inf floats are an absolute no-go on the wire: ArduinoJson's String
    // ctor prints "nan"/"inf" which is not valid JSON, the server rejects it,
    // and our heap budget is too tight to recover by retrying. Downgrade to 0
    // so the row is still parsable and the dashboard can flag the missing
    // data on the channel.
    if (!isfinite(v)) v = 0.0f;
    obj[key] = String((double)v, FLOAT_DECIMALS);
}

static void serialize_telemetry_core(const TelemetrySnapshot& s, JsonObject root) {
    root["v"] = s.schema_version;          // schema version (uint8)
    root["schema"] = s.schema;             // "telemetry_v1"

    // ts: 0 when NTP has never synced, otherwise the epoch seconds from
    // telemetry_build(). The dashboard distinguishes the two via the
    // time_source field below; the contract is documented in
    // docs/API.md and the server treats ts=0 as "data is real but the
    // wall-clock is not yet trustworthy."
    root["ts"] = s.ts;
    root["ts_ms"] = s.ts_ms;
    if (s.ts == 0 || !g_ntp_synced) {
        root["time_source"] = "uptime";
    } else {
        root["time_source"] = "ntp";
    }

    JsonObject dev = root["device"].to<JsonObject>();
    dev["id"] = s.device.id;
    dev["fw"] = s.device.fw;
    dev["uptime_ms"] = s.device.uptime_ms;

    JsonObject wifi = root["wifi"].to<JsonObject>();
    wifi["rssi"] = s.wifi.rssi;
    wifi["ip"] = s.wifi.ip;

    JsonArray chans = root["channels"].to<JsonArray>();
    for (uint8_t i = 0; i < s.channel_count; i++) {
        const TelemetryChannel& c = s.channels[i];
        JsonObject co = chans.add<JsonObject>();
        co["ch"] = c.ch;
        write_float(co, "V", c.V);
        write_float(co, "I", c.I);
        write_float(co, "P", c.P);
        write_float(co, "energy_Wh", c.energy_Wh);
        write_float(co, "charge_mAh", c.charge_mAh);
    }

    JsonArray sws = root["switches"].to<JsonArray>();
    for (uint8_t i = 0; i < s.switch_count; i++) {
        const TelemetrySwitch& sw = s.switches[i];
        JsonObject so = sws.add<JsonObject>();
        so["idx"] = sw.idx;
        so["type"] = sw.type;
        so["state"] = sw.state;
        so["auto"] = sw.auto_mode;
        so["rule_tripped"] = sw.rule_tripped;
    }

    JsonArray bats = root["battery"].to<JsonArray>();
    for (uint8_t i = 0; i < s.battery_count; i++) {
        const TelemetryBattery& b = s.battery[i];
        JsonObject bo = bats.add<JsonObject>();
        bo["ch"] = b.ch;
        bo["profile_id"] = b.profile_id;
        bo["chemistry"] = b.chemistry;
        write_float(bo, "rated_Ah", b.rated_Ah);
        write_float(bo, "soc_pct", b.soc_pct);
        write_float(bo, "V", b.V);
        write_float(bo, "I", b.I);
        write_float(bo, "cumulative_Ah_in", b.cumulative_Ah_in);
        write_float(bo, "cumulative_Ah_out", b.cumulative_Ah_out);
        write_float(bo, "equivalent_full_cycles", b.equivalent_full_cycles);
        bo["capacity_test_active"] = b.capacity_test_active;
        if (b.capacity_test_soh_valid) {
            write_float(bo, "capacity_test_soh_pct", b.capacity_test_soh_pct);
        }
    }

    JsonObject log = root["log"].to<JsonObject>();
    log["entries"] = s.log.entries;
    log["overflow"] = s.log.overflow;

    root["heap_free"] = s.heap_free;
}

// Serialize the full snapshot into a caller-provided buffer. Returns the
// number of bytes written (excludes the null terminator), or 0 on overflow.
static size_t serialize_telemetry(const TelemetrySnapshot& s, char* out, size_t out_len) {
    if (!out || out_len < 256) return 0;
    JsonDocument doc;
    serialize_telemetry_core(s, doc.to<JsonObject>());
    size_t n = serializeJson(doc, out, out_len);
    if (n == 0 || n >= out_len) return 0;
    return n;
}

// Build a small BLE-friendly subset: ts + channels + battery. The full
// payload doesn't fit BLE MTU at default channel count; the subset keeps
// core metrics and drops switches/wifi/heap/log. Targets <= 200 bytes.
static size_t serialize_telemetry_ble(const TelemetrySnapshot& s, char* out, size_t out_len) {
    if (!out || out_len < 128) return 0;
    JsonDocument doc;
    doc["v"] = s.schema_version;
    doc["ts"] = s.ts;
    JsonArray chans = doc["channels"].to<JsonArray>();
    for (uint8_t i = 0; i < s.channel_count; i++) {
        const TelemetryChannel& c = s.channels[i];
        JsonObject co = chans.add<JsonObject>();
        co["ch"] = c.ch;
        write_float(co, "V", c.V);
        write_float(co, "I", c.I);
        write_float(co, "P", c.P);
        co["c"] = (int32_t)c.charge_mAh;  // int mAh saves bytes in BLE MTU
    }
    JsonArray bats = doc["battery"].to<JsonArray>();
    for (uint8_t i = 0; i < s.battery_count; i++) {
        const TelemetryBattery& b = s.battery[i];
        JsonObject bo = bats.add<JsonObject>();
        bo["ch"] = b.ch;
        write_float(bo, "soc_pct", b.soc_pct);
    }
    size_t n = serializeJson(doc, out, out_len);
    if (n == 0 || n >= out_len) return 0;
    return n;
}

// Battery profiles heartbeat — published on a slow path (60s) and on profile
// change. Built ad-hoc from settings_manager (does NOT use TelemetrySnapshot
// fields because the profile list is slow-changing config, not per-sample
// data). The server gets the same shape regardless of how the heartbeat was
// triggered.
static uint32_t g_last_profiles_pub_ms = 0;
static uint32_t g_last_profiles_hash = 0;

static void publish_battery_profiles_heartbeat(const char* supabase_url,
                                               const char* anon_key) {
    if (skip_network) return;
    char device_key[64], api_key[64];
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;
    if (!is_valid_uuid(api_key)) return;

    // Build the profile list. Hash the names+chem+cap so we can detect changes
    // and publish eagerly when something changes (caller drives this path).
    //
    // Wire format: the migration's `sync_battery_profiles(p_device_key text,
    // p_profiles jsonb)` expects `{p_device_key, p_profiles}`. The PostgREST
    // RPC passes the entire request body as the function's named arguments.
    JsonDocument doc;
    doc["p_device_key"] = device_key;
    JsonArray arr = doc["p_profiles"].to<JsonArray>();

    uint32_t hash = 0;
    for (uint8_t ch = 0; ch < BATTERY_MAX_PROFILES; ch++) {
        const BatteryChemistryProfile* bp = battery_profile_get(ch);
        if (!bp) continue;
        JsonObject o = arr.add<JsonObject>();
        o["id"] = bp->id;
        o["name"] = bp->name;
        o["chemistry"] = battery_chemistry_name(bp->chemistry);
        o["nominal_voltage"] = String((double)bp->nominal_voltage, FLOAT_DECIMALS);
        o["rated_capacity_Ah"] = String((double)bp->rated_capacity_Ah, FLOAT_DECIMALS);
        o["c_rating"] = String((double)bp->c_rating, FLOAT_DECIMALS);
        o["cutoff_voltage"] = String((double)bp->cutoff_voltage, FLOAT_DECIMALS);
        o["float_voltage"] = String((double)bp->float_voltage, FLOAT_DECIMALS);
        o["charge_efficiency"] = String((double)bp->charge_efficiency, FLOAT_DECIMALS);
        o["cycle_life_rated"] = bp->cycle_life_rated;
        o["min_soc_pct"] = String((double)bp->min_soc_pct, FLOAT_DECIMALS);
        o["max_soc_pct"] = String((double)bp->max_soc_pct, FLOAT_DECIMALS);
        // FNV-1a over key fields
        const char* p = bp->name;
        while (*p) hash = (hash * 16777619u) ^ (uint8_t)*p++;
        hash = (hash * 16777619u) ^ (uint8_t)bp->chemistry;
        hash = (hash * 16777619u) ^ (uint8_t)(bp->rated_capacity_Ah * 1000.0f);
    }

    static char buf[1024];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    if (len == 0) return;

    // Always publish on a 60s cadence. If a profile changed (hash differs),
    // reset the timer so the next 60s window restarts from now.
    uint32_t now = millis();
    bool due = (now - g_last_profiles_pub_ms) >= 60000UL;
    bool changed = (hash != g_last_profiles_hash);
    if (!due && !changed) return;
    g_last_profiles_pub_ms = now;
    g_last_profiles_hash = hash;

    telemetry_post("/rest/v1/rpc/sync_battery_profiles", buf, len, supabase_url, anon_key);
}

// Force a battery_profiles publish on the next opportunity. Call this from
// settings_command handlers (set_battery, set_battery_profile, etc.) so the
// dashboard sees new profile data without waiting 60s.
void telemetry_kick_battery_profiles() {
    g_last_profiles_pub_ms = 0;
    g_last_profiles_hash = 0;
}

static JsonDocument g_supa_doc;

#define LOG_BATCH_SIZE 10
// send_log_entry / flush_log_batch each construct their own local
// StaticJsonDocument. The previous static g_log_doc was shared state that
// every caller had to remember to .clear() — easy to miss, and a corruption
// source if any path forgets. The local doc costs a small heap frame per
// call but is bounded (~1 KB) and confined to the single network task.
static uint8_t g_log_count = 0;
static uint32_t g_log_last_ts = 0;

static void flush_log_batch(const char* supabase_url, const char* anon_key,
    const char* device_key, const char* api_key) {
    if (g_log_count == 0) return;

    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();

    for (uint8_t e = 0; e < g_log_count; e++) {
        JsonObject elem = arr.add<JsonObject>();
        elem["p_device_key"] = device_key;
        elem["p_device_api_key"] = api_key;
        elem["p_recorded_at"] = g_log_last_ts + e;  // approximate per-entry timestamps
        elem["p_metadata"] = JsonObject();
    }

    // Heap-allocated buffer to keep the network task's stack frame small.
    // LOG_BATCH_SIZE entries * ~300 bytes each ≈ 3 KB worst case.
    size_t needed = serializeJson(doc, nullptr, 0) + 16;
    uint8_t* buffer = (uint8_t*)malloc(needed);
    if (!buffer) {
        LOG_PRINTLN("[LOG] OOM in flush_log_batch — dropping batch");
        g_log_count = 0;
        return;
    }
    size_t len = serializeJson(doc, buffer, needed);
    telemetry_post("/rest/v1/rpc/insert_telemetry", (char*)buffer, len, supabase_url, anon_key);
    free(buffer);
    g_log_count = 0;
}

static void send_log_entry(uint32_t timestamp_ms, const int16_t* v, const int16_t* i, const int16_t* p,
    const char* entry_type, const char* supabase_url, const char* anon_key,
    const char* device_key, const char* api_key) {
    if (!is_valid_uuid(api_key)) return;

    // Local doc per call (see fix-46 comment above g_log_count).
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();

    JsonObject elem = arr.add<JsonObject>();
    elem["p_device_key"] = device_key;
    elem["p_device_api_key"] = api_key;

    JsonObject payload = elem["p_payload"].to<JsonObject>();
    payload["source"] = "log";
    payload["entry_type"] = entry_type;
    payload["ina3221_v0"] = v[0] / 1000.0f;
    payload["ina3221_v1"] = v[1] / 1000.0f;
    payload["ina3221_v2"] = v[2] / 1000.0f;
    payload["ina3221_i0"] = i[0] / 1000.0f;
    payload["ina3221_i1"] = i[1] / 1000.0f;
    payload["ina3221_i2"] = i[2] / 1000.0f;
    payload["ch0_P"] = (v[0] * i[0]) / 1000000.0f;
    payload["ch1_P"] = (v[1] * i[1]) / 1000000.0f;
    payload["ch2_P"] = (v[2] * i[2]) / 1000000.0f;
#if ENABLE_INA226
    payload["ina226_v"] = v[3] / 1000.0f;
    payload["ina226_i"] = i[3] / 1000.0f;
    payload["ina226_p"] = p[3] / 1.0f;
    payload["ch3_P"] = p[3] / 1.0f;
#endif
    payload["log_entries"] = log_entries_count();
    payload["log_buffer_kb"] = log_buffer_capacity() / 1024;
    payload["log_overflow"] = log_has_overflow_file();
    payload["log_overflow_bytes"] = log_overflow_file_size();

    for (uint8_t ch = 0; ch < 4; ch++) {
        char key[16];
        snprintf(key, sizeof(key), "relay%d", ch);
        payload[key] = get_switch_state(ch);
    }

    {
        time_t ts = log_to_epoch(timestamp_ms);
        if (ts == (time_t)-1) {
            // log_epoch_valid was false at the moment of encoding. Stamp 0
            // so the consumer knows the timestamp is untrusted, and warn
            // once per log batch.
            elem["p_recorded_at"] = 0;
            g_log_last_ts = 0;
            LOG_PRINTLN("log timestamp invalid; stamping 0");
        } else {
            elem["p_recorded_at"] = (uint32_t)ts;
            g_log_last_ts = (uint32_t)ts;
        }
    }
    g_log_count++;

    if (g_log_count >= LOG_BATCH_SIZE) {
        flush_log_batch(supabase_url, anon_key, device_key, api_key);
    }
}

static size_t decode_and_send_log_entries(const uint8_t* data, size_t len,
    const char* supabase_url, const char* anon_key,
    const char* device_key, const char* api_key,
    int16_t* abs_v, int16_t* abs_i, int16_t* abs_p, uint32_t* abs_ts, bool* have_abs) {
    size_t offset = 0;
    while (offset < len) {
        uint8_t type = data[offset];
        if (type == ENTRY_BASE) {
            if (offset + sizeof(BaseEntry) > len) break;
            BaseEntry e;
            memcpy(&e, data + offset, sizeof(e));
            *abs_ts = e.timestamp_ms;
            for (int ch = 0; ch < 4; ch++) {
                abs_v[ch] = e.v[ch];
                abs_i[ch] = e.i[ch];
                abs_p[ch] = e.p[ch];
            }
            *have_abs = true;
            send_log_entry(*abs_ts, abs_v, abs_i, abs_p, "base",
                supabase_url, anon_key, device_key, api_key);
            offset += sizeof(BaseEntry);
        } else if (type == ENTRY_DELTA) {
            if (offset + sizeof(DeltaEntry) > len) break;
            if (!*have_abs) { offset++; continue; }
            DeltaEntry e;
            memcpy(&e, data + offset, sizeof(e));
            *abs_ts += e.dt_ms;
            for (int ch = 0; ch < 4; ch++) {
                abs_v[ch] += e.dv[ch];
                abs_i[ch] += e.di[ch];
                abs_p[ch] += e.dp[ch];
            }
            send_log_entry(*abs_ts, abs_v, abs_i, abs_p, "delta",
                supabase_url, anon_key, device_key, api_key);
            offset += sizeof(DeltaEntry);
        } else {
            offset++;
        }
    }
    return offset;
}

void publish_log_batch_supabase() {
    if (skip_network) return;
    static unsigned long last_log_pub_ms = 0;
    if (millis() - last_log_pub_ms < 1000) return; // rate limit: 1 call per second
    last_log_pub_ms = millis();

    // 8KB minimum — safe for g_supa_doc (~2KB) + stack buffers in this function
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_LOWFREQ) {
        LOG_PRINTLN("[WARN] Low heap, skipping Supabase log publish");
        return;
    }
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;
    if (!is_valid_uuid(api_key)) return;

    // Static state machine: drains entries per call to keep up with 1s sensor rate
    static enum { ST_RAM, ST_FS, ST_DONE } state = ST_RAM;
    static int16_t ram_v[4] = {0}, ram_i[4] = {0}, ram_p[4] = {0};
    static uint32_t ram_ts = 0;
    static bool ram_have = false;
    static uint8_t ram_carry[sizeof(BaseEntry)];
    static size_t ram_carry_len = 0;
    static int16_t fs_v[4] = {0}, fs_i[4] = {0}, fs_p[4] = {0};
    static uint32_t fs_ts = 0;
    static bool fs_have = false;
    static uint8_t fs_carry[sizeof(BaseEntry)];
    static size_t fs_carry_len = 0;
    static bool fs_file_open = false;

    if (state == ST_RAM) {
        // Drain up to 5 entries per call to outpace 1s sensor arrival rate
        for (int sent = 0; sent < 5; sent++) {
            uint8_t chunk[sizeof(BaseEntry)];
            size_t n = log_pop_batch(chunk, sizeof(chunk));
            if (n == 0) {
                state = ST_FS;
                break;
            }
            uint8_t work[sizeof(BaseEntry) * 2];
            memcpy(work, ram_carry, ram_carry_len);
            memcpy(work + ram_carry_len, chunk, n);
            size_t work_len = ram_carry_len + n;

            size_t consumed = decode_and_send_log_entries(work, work_len, supabase_url, anon_key, device_key, api_key,
                ram_v, ram_i, ram_p, &ram_ts, &ram_have);

            ram_carry_len = work_len - consumed;
            if (ram_carry_len > 0) {
                memcpy(ram_carry, work + consumed, ram_carry_len);
            }
        }
        // Immediately transition to FS if RAM drained — no waiting for next tick
        if (log_entries_count() == 0 && state == ST_RAM) {
            state = ST_FS;
        }
    }

    if (state == ST_FS) {
        if (!fs_file_open) {
            if (!log_open_overflow_for_read()) {
                state = ST_DONE;
                return;
            }
            fs_file_open = true;
        }
        // Drain up to 5 FS chunks per call
        for (int sent = 0; sent < 5; sent++) {
            uint8_t chunk[512];
            size_t n = log_read_overflow_chunk(chunk, sizeof(chunk));
            if (n == 0) {
                if (fs_carry_len > 0) {
                    decode_and_send_log_entries(fs_carry, fs_carry_len, supabase_url, anon_key, device_key, api_key,
                        fs_v, fs_i, fs_p, &fs_ts, &fs_have);
                    fs_carry_len = 0;
                }
                log_close_overflow();
                fs_file_open = false;
                state = ST_DONE;
                break;
            }
            uint8_t work[512 + sizeof(BaseEntry)];
            memcpy(work, fs_carry, fs_carry_len);
            memcpy(work + fs_carry_len, chunk, n);
            size_t work_len = fs_carry_len + n;

            size_t consumed = decode_and_send_log_entries(work, work_len, supabase_url, anon_key, device_key, api_key,
                fs_v, fs_i, fs_p, &fs_ts, &fs_have);

            fs_carry_len = work_len - consumed;
            if (fs_carry_len > 0) {
                memcpy(fs_carry, work + consumed, fs_carry_len);
            }
        }
        // Immediately transition to DONE if no more FS data — no waiting for next tick
        if (state == ST_FS && fs_carry_len == 0 && !log_has_overflow_file()) {
            state = ST_DONE;
        }
    }

    if (state == ST_DONE) {
        flush_log_batch(supabase_url, anon_key, device_key, api_key);
        if (log_entries_count() > 0 || log_has_overflow_file()) {
            state = ST_RAM;
            ram_have = false;
            ram_carry_len = 0;
            fs_have = false;
            fs_carry_len = 0;
        }
    }
}

static JsonDocument g_pub_doc;

void publish_data(const SensorSnapshot& data) {
    (void)data;
    if (skip_network) return;

    // Build the canonical snapshot from all current sources. We do not depend
    // on the SensorSnapshot argument here because telemetry_build() pulls
    // fresh data from sensor_manager / counters / NVS at the moment of
    // publish, which is the right semantic for a 5-s publish tick.
    TelemetrySnapshot snap;
    telemetry_build(snap);

#if MQTT_LEGACY_PAYLOAD
    // Legacy payload shape preserved for existing MQTT consumers. Re-emits
    // the ina3221/ina226/ads1115/ch_N_V/relayN/log_* keys exactly as before.
    g_pub_doc.clear();
    JsonArray ina3221Arr = g_pub_doc["ina3221"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
        JsonObject ch = ina3221Arr.add<JsonObject>();
        ch["v"] = isfinite(get_channel_voltage(i))   ? get_channel_voltage(i)   : 0.0f;
        ch["i"] = isfinite(get_channel_current(i))   ? get_channel_current(i)   : 0.0f;
    }
#if ENABLE_INA226
    JsonObject ina226Obj = g_pub_doc["ina226"].to<JsonObject>();
    ina226Obj["v"] = isfinite(get_channel_voltage(3)) ? get_channel_voltage(3) : 0.0f;
    ina226Obj["i"] = isfinite(get_channel_current(3)) ? get_channel_current(3) : 0.0f;
    ina226Obj["p"] = isfinite(get_channel_power(3))   ? get_channel_power(3)   : 0.0f;
#endif
    JsonArray adcArr = g_pub_doc["ads1115"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) {
        float av = get_channel_voltage(i);
        adcArr.add(isfinite(av) ? av : 0.0f);
    }
    g_pub_doc["log_entries"] = log_entries_count();
    g_pub_doc["log_buffer_kb"] = log_buffer_capacity() / 1024;
    g_pub_doc["log_overflow"] = log_has_overflow_file();
    g_pub_doc["log_overflow_bytes"] = log_overflow_file_size();
    for (uint8_t i = 0; i < 4; i++) {
        char key[16];
        snprintf(key, sizeof(key), "relay%d", i);
        g_pub_doc[key] = get_switch_state(i);
    }
    for (uint8_t ch = 0; ch < 4; ch++) {
        VirtualChannelConfig vc;
        char key[16];
        float v = 0, i = 0, p = 0;
        if (settings_load_virtual_channel(ch, &vc) && (vc.voltage_src > 0 || vc.current_src > 0)) {
            if (vc.voltage_src > 0) v = get_sensor_voltage(vc.voltage_src, vc.voltage_idx, data);
            if (vc.current_src > 0) {
                i = get_sensor_current(vc.current_src, vc.current_idx, data);
                if (vc.current_src == 3) p = get_sensor_power(vc.current_src, vc.current_idx, data);
                else if (vc.voltage_src > 0) p = v * i;
            }
        } else if (ch < 3) {
            v = get_channel_voltage(ch);
            i = get_channel_current(ch);
            p = v * i;
        } else {
            v = get_channel_voltage(3);
            i = get_channel_current(3);
            p = get_channel_power(3);
        }
        snprintf(key, sizeof(key), "ch%d_V", ch); g_pub_doc[key] = isfinite(v) ? v : 0.0f;
        snprintf(key, sizeof(key), "ch%d_I", ch); g_pub_doc[key] = isfinite(i) ? i : 0.0f;
        snprintf(key, sizeof(key), "ch%d_P", ch); g_pub_doc[key] = isfinite(p) ? p : 0.0f;
    }
    char buffer[1024];
    size_t len = serializeJson(g_pub_doc, buffer, sizeof(buffer));
#else
    // New shape: serialize TelemetrySnapshot into a 4 KB buffer.
    static char buffer[TELEMETRY_BUF_BYTES];
    size_t len = serialize_telemetry(snap, buffer, sizeof(buffer));
    if (len == 0) {
        // Should not happen at full saturation; bail to avoid publishing a
        // truncated JSON.
        return;
    }
#endif
#if CORE_DEBUG_LEVEL >= 3
    LOG_PRINT("[TELEM] %u bytes (ch=%u sw=%u bat=%u heap=%u)\n",
        (unsigned)len, snap.channel_count, snap.switch_count, snap.battery_count,
        ESP.getFreeHeap());
#endif

    // MQTT — keep the legacy 1-second rate limit so the broker doesn't drown
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    static unsigned long last_mqtt_pub = 0;
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        if (mqtt.connected() && millis() - last_mqtt_pub >= 1000) {
            last_mqtt_pub = millis();
            if (!mqtt.publish(MQTT_TOPIC, buffer, len)) {
#if CORE_DEBUG_LEVEL >= 3
                LOG_PRINTLN("[MQTT] publish() returned false — broker dropped or buffer full");
#endif
            }
        }
    }

    publish_data_http(data, buffer, len);

    // Blynk virtual writes disabled — enable via platformio.ini lib_deps

    // BLE: small subset (channels + battery + ts) to fit default MTU.
    static char ble_buf[256];
    size_t blen = serialize_telemetry_ble(snap, ble_buf, sizeof(ble_buf));
    if (blen > 0) {
        ble_notify_sensor_data(ble_buf, blen);
    } else {
        // Fall back to full buffer if subset serializer fails (shouldn't, but
        // keeps the notify path resilient).
        ble_notify_sensor_data(buffer, len);
    }
    vTaskDelay(pdMS_TO_TICKS(25));  // space out notifies — avoids BLE stack crowding / UX jitter
}

void publish_data_supabase(const SensorSnapshot& data) {
    (void)data;
    if (skip_network) return;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PUBLISH) {
        static unsigned long last_warn = 0;
        if (millis() - last_warn > 10000) {
            LOG_PRINT("[WARN] Low heap (%d / largest=%d), skipping Supabase publish\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            last_warn = millis();
        }
        return;
    }

    // The current path publishes one canonical TelemetrySnapshot per call —
    // the legacy batching buffer (g_batch[]) and the `if (g_batch_count < 1)
    // return;` dead-storage check were removed. The SensorSnapshot argument
    // is accepted for caller compatibility (main.cpp pushes a 1Hz queue
    // entry) but telemetry_build() pulls fresh data from sensor_manager /
    // counters / NVS at the moment of publish, which is the right semantic
    // for a 5-s publish tick.

    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;
    if (!is_valid_uuid(api_key)) {
        LOG_PRINTLN("[WARN] device_api_key is not a valid UUID — skip Supabase publish");
        return;
    }

    // Build the canonical snapshot once and serialize it.
    TelemetrySnapshot snap;
    telemetry_build(snap);

    // Wrap it in the Supabase RPC envelope: array of {p_device_key,
    // p_device_api_key, p_payload, p_recorded_at}. One row per call keeps the
    // server-side insert trivial.
    g_supa_doc.clear();
    JsonArray arr = g_supa_doc.to<JsonArray>();
    JsonObject elem = arr.add<JsonObject>();
    elem["p_device_key"] = device_key;
    elem["p_device_api_key"] = api_key;
    JsonObject payload = elem["p_payload"].to<JsonObject>();
    serialize_telemetry_core(snap, payload);
    elem["p_recorded_at"] = snap.ts;

#if CORE_DEBUG_LEVEL >= 3
    LOG_PRINT("[TELEM/SUPA] v=%u ts=%u %u bytes (heap=%u)\n",
        snap.schema_version, snap.ts,
        (unsigned)serializeJson(g_supa_doc, nullptr, 0), ESP.getFreeHeap());
#endif

    // Heap-allocated buffer to keep this function's stack frame small.
    size_t needed = serializeJson(g_supa_doc, nullptr, 0) + 16;
    if (needed > 16384) {
        LOG_PRINT("[SUPA] payload too large: %u bytes — dropping\n", (unsigned)needed);
        return;
    }
    char* buffer = (char*)malloc(needed);
    if (!buffer) {
        LOG_PRINTLN("[SUPA] OOM serializing telemetry — dropping publish");
        return;
    }
    size_t len = serializeJson(g_supa_doc, buffer, needed);

    int rc = telemetry_post("/rest/v1/rpc/insert_telemetry", buffer, len, supabase_url, anon_key);
    if (rc == 200 || rc == 201 || rc == 204) {
        // Network confirmed up and reachable — clear any stale overflow file
        // since entries were captured in RAM and have now been published.
        if (log_has_overflow_file()) {
            log_close_overflow();
        }
    } else {
        print_http_error(g_supa_http, rc);
        if (rc == 400) {
            LOG_PRINTLN("Payload preview: ");
            LOG_PRINTLN(buffer);
        }
    }
    free(buffer);

    // Slow path: battery profile heartbeat (60s) and eager on change.
    publish_battery_profiles_heartbeat(supabase_url, anon_key);

    publish_calibration_status();
}

// g_rpc_doc — shared RPC payload buffer for the network task.
// IMPORTANT: This document is owned by the network task ONLY. It is used to
// build RPC envelopes for sync_device_channels_to_supabase() and (in the
// future) other sync paths. Any other thread that needs to build an RPC
// payload MUST construct its own local StaticJsonDocument. The state machine
// is: claim_settings_command -> apply -> sync_device_channels -> back to
// loop. Nothing in that sequence re-enters RPC construction concurrently, so
// a single static document is safe within the network task.
static JsonDocument g_rpc_doc;

// Sync full device_channels config to Supabase after any settings change.
// This keeps Supabase device_channels row in sync with ESP32 NVS so the
// dashboard UI sees up-to-date values after any config command.
static void sync_device_channels_to_supabase() {
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_LOWFREQ) return;
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    settings_load_supabase_api_key(api_key, sizeof(api_key));

    g_rpc_doc.clear();
    g_rpc_doc["device_key"] = device_key;

    // Channel names
    JsonArray names = g_rpc_doc["channel_names"].to<JsonArray>();
    for (uint8_t ch = 0; ch < 4; ch++) {
        JsonObject n = names.add<JsonObject>();
        n["channel"] = ch;
        char name[24] = "";
        settings_load_channel_name(ch, name, sizeof(name));
        n["name"] = name;
    }

    // Battery profiles
    JsonArray bats = g_rpc_doc["battery_profiles"].to<JsonArray>();
    const char* CHEM[] = { "lead_acid", "lipol", "liion", "nimh", "lifepo4", "agm", "fla" };
    for (uint8_t ch = 0; ch < 4; ch++) {
        BatteryProfile bp;
        if (settings_load_battery_profile(ch, &bp)) {
            JsonObject b = bats.add<JsonObject>();
            b["channel"] = ch;
            b["name"] = bp.name;
            b["chemistry"] = bp.chemistry < 7 ? CHEM[bp.chemistry] : "lead_acid";
            b["system_voltage"] = bp.system_voltage;
            b["capacity_mAh"] = bp.capacity_mAh;
            b["initial_soc_pct"] = bp.initial_soc_pct;
            b["cell_count"] = bp.cell_count;
            b["full_voltage"] = bp.full_voltage;
            b["cutoff_voltage"] = bp.cutoff_voltage;
            b["float_voltage"] = bp.float_voltage;
        }
    }

    // Channel groups
    JsonArray groups = g_rpc_doc["channel_groups"].to<JsonArray>();
    uint8_t gc = settings_load_channel_group_count();
    for (uint8_t i = 0; i < gc; i++) {
        ChannelGroup cg;
        if (settings_load_channel_group(i, &cg)) {
            JsonObject g = groups.add<JsonObject>();
            g["group_id"] = cg.group_id;
            g["name"] = cg.name;
            g["icon"] = cg.icon;
            g["channel_mask"] = cg.channel_mask;
        }
    }

    // Virtual channels
    JsonArray vcs = g_rpc_doc["virtual_channels"].to<JsonArray>();
    for (uint8_t ch = 0; ch < 4; ch++) {
        VirtualChannelConfig vc;
        if (settings_load_virtual_channel(ch, &vc)) {
            JsonObject v = vcs.add<JsonObject>();
            v["channel"] = ch;
            v["voltage_src"] = vc.voltage_src;
            v["voltage_idx"] = vc.voltage_idx;
            v["current_src"] = vc.current_src;
            v["current_idx"] = vc.current_idx;
        }
    }

    // Calibration
    ChannelCalibration cal;
    if (settings_load_channel_calibration(&cal)) {
        JsonObject cal_obj = g_rpc_doc["channel_calibration"].to<JsonObject>();
        JsonArray volt_offset = cal_obj["volt_offset_mv"].to<JsonArray>();
        JsonArray volt_gain = cal_obj["volt_gain"].to<JsonArray>();
        JsonArray curr_offset = cal_obj["curr_offset_ma"].to<JsonArray>();
        JsonArray curr_gain = cal_obj["curr_gain"].to<JsonArray>();
        JsonArray invert_curr = cal_obj["invert_curr"].to<JsonArray>();
        for (uint8_t i = 0; i < 3; i++) {
            volt_offset.add(cal.volt_offset_mv[i]);
            volt_gain.add(cal.volt_gain[i]);
            curr_offset.add(cal.curr_offset_ma[i]);
            curr_gain.add(cal.curr_gain[i]);
            invert_curr.add(cal.invert_curr[i]);
        }
    }

    static char buffer[1024];
    size_t len = serializeJson(g_rpc_doc, buffer);

    // Use RPC for device sync (same security definer pattern as claim_settings_command)
    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/rpc/sync_device_channels");

    // Build RPC body: {p_device_key: "...", p_payload: {...}}
    JsonDocument rpc_doc;
    rpc_doc["p_device_key"] = device_key;
    // Embed the full device_channels payload directly (g_rpc_doc is still intact here)
    rpc_doc["p_payload"]["channel_names"] = g_rpc_doc["channel_names"];
    rpc_doc["p_payload"]["battery_profiles"] = g_rpc_doc["battery_profiles"];
    rpc_doc["p_payload"]["channel_calibration"] = g_rpc_doc["channel_calibration"];
    rpc_doc["p_payload"]["virtual_channels"] = g_rpc_doc["virtual_channels"];
    rpc_doc["p_payload"]["channel_groups"] = g_rpc_doc["channel_groups"];

    len = serializeJson(rpc_doc, buffer);
    int rc = supabase_post(path, buffer, len, supabase_url, anon_key);
    if (rc >= 200 && rc < 300) {
        LOG_PRINTLN("[DB] device_channels synced to Supabase");
    } else {
        LOG_PRINT("[DB] device_channels sync failed: %d\n", rc);
    }
}

void sync_calibration_to_supabase() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_SYNC_OPS) return;
    telemetry_http_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    ChannelCalibration cal;
    if (!settings_load_channel_calibration(&cal)) return;

    JsonDocument rpc_doc;
    rpc_doc["p_device_key"] = device_key;
    JsonObject cal_obj = rpc_doc["p_calibration"].to<JsonObject>();
    JsonArray volt_offset = cal_obj["volt_offset_mv"].to<JsonArray>();
    JsonArray volt_gain = cal_obj["volt_gain"].to<JsonArray>();
    JsonArray curr_offset = cal_obj["curr_offset_ma"].to<JsonArray>();
    JsonArray curr_gain = cal_obj["curr_gain"].to<JsonArray>();
    JsonArray invert_curr = cal_obj["invert_curr"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
        volt_offset.add(cal.volt_offset_mv[i]);
        volt_gain.add(cal.volt_gain[i]);
        curr_offset.add(cal.curr_offset_ma[i]);
        curr_gain.add(cal.curr_gain[i]);
        invert_curr.add(cal.invert_curr[i]);
    }

    static char buffer[512];
    size_t len = serializeJson(rpc_doc, buffer);
    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/rpc/sync_device_calibration");
    int rc = supabase_post(path, buffer, len, supabase_url, anon_key);
    if (rc >= 200 && rc < 300) {
        LOG_PRINTLN("Calibration synced to Supabase");
    } else {
        LOG_PRINT("Calibration sync failed: %d\n", rc);
    }
}

void sync_ble_pin_to_supabase() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_SYNC_OPS) return;
    telemetry_http_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    uint32_t pin = settings_load_ble_pin();
    char pin_str[16];
    snprintf(pin_str, sizeof(pin_str), "%lu", (unsigned long)pin);

    // Use security definer RPC to bypass RLS
    JsonDocument rpc_doc;
    rpc_doc["p_device_key"] = device_key;
    rpc_doc["p_ble_pin"] = pin_str;
    static char buffer[256];
    size_t len = serializeJson(rpc_doc, buffer);
    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/rpc/sync_ble_pin");
    int rc = supabase_post(path, buffer, len, supabase_url, anon_key);
    if (rc >= 200 && rc < 300) {
        LOG_PRINTLN("BLE PIN synced to Supabase");
    } else {
        LOG_PRINT("BLE PIN sync FAILED: HTTP %d\n", rc);
    }
}

void publish_switch_state(uint8_t idx, bool is_energized) {
    if (skip_network) { LOG_PRINTLN("[SWITCH] skip: offline mode"); return; }
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_SWITCH) { LOG_PRINT("[SWITCH] skip: heap %d < %u\n", ESP.getFreeHeap(), (unsigned)MIN_FREE_HEAP_FOR_SWITCH); return; }
    telemetry_http_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    char supabase_url[128], anon_key[128], device_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) { LOG_PRINTLN("[SWITCH] skip: no supabase url"); return; }
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) { LOG_PRINTLN("[SWITCH] skip: no anon key"); return; }
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) { LOG_PRINTLN("[SWITCH] skip: no device key"); return; }

    SwitchChannel ch;
    SwitchRule rule;
    if (!settings_load_switch(idx, &ch)) { LOG_PRINT("[SWITCH] skip: no switch config idx=%d\n", idx); return; }
    settings_load_switch_rule(idx, &rule);

    // Use security definer RPC to bypass RLS
    JsonDocument rpc_doc;
    rpc_doc["p_device_key"] = device_key;
    rpc_doc["p_switch_index"] = idx;
    rpc_doc["p_gpio_pin"] = ch.gpio_pin;
    rpc_doc["p_is_energized"] = is_energized;
    rpc_doc["p_active_high"] = ch.active_high;
    rpc_doc["p_switch_type"] = ch.type;
    // NOTE: do NOT send p_last_tripped_at. The Postgres column has `default now()`,
    // and the literal string "now" we used to send is not a valid timestamptz.
    // The server-side default fills in the real wall-clock timestamp; if the
    // device is not NTP-synced, that's still better than the string "now".
    rpc_doc["p_channel"] = rule.channel;

    static char buffer[256];
    size_t len = serializeJson(rpc_doc, buffer);
    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/rpc/sync_switch_state");
    int rc = supabase_post(path, buffer, len, supabase_url, anon_key);
    if (rc >= 200 && rc < 300) {
        LOG_PRINT("[SWITCH] synced: idx=%d energized=%d\n", idx, is_energized);
    } else {
        LOG_PRINT("[SWITCH] sync FAILED: HTTP %d\n", rc);
    }
}

void apply_settings_posthook(const char* cmd_type) {
    if (strcmp(cmd_type, "set_wifi") == 0) {
        char ssid[64] = "", pass[64] = "";
        if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
            WiFi.disconnect(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            WiFi.begin(ssid, pass);
            LOG_PRINTLN("[CMD] WiFi reconnecting with new credentials");
        }
    } else if (strcmp(cmd_type, "set_mqtt") == 0) {
        char broker[64], topic[64];
        uint16_t port;
        if (settings_load_mqtt(broker, &port, topic, sizeof(broker))) {
            mqtt.disconnect();
            mqtt.setServer(broker, port);
            LOG_PRINTLN("[CMD] MQTT reconnecting with new broker");
        }
    } else if (strcmp(cmd_type, "set_supabase") == 0) {
        supabase_http_reset();
        LOG_PRINTLN("[CMD] Supabase client reset with new URL/key");
    } else if (strcmp(cmd_type, "set_shunt") == 0 || strcmp(cmd_type, "set_volt_ratio") == 0 || strcmp(cmd_type, "set_resistors") == 0) {
        reinit_sensors();
        LOG_PRINTLN("[CMD] Sensor params reloaded from NVS");
    }
    // sync_device_channels_to_supabase() is called by check_settings_commands after apply_settings_command returns
    // so no need to call it here for most commands
    // (calibrate_baseline calls sync_calibration_to_supabase directly in apply_settings_command)
}

void publish_calibration_status() {
    if (skip_network) return;
    if (!sensor_is_calibrating()) return;  // nothing to report
    static unsigned long last_cal_pub_ms = 0;
    if (millis() - last_cal_pub_ms < 5000) return; // rate limit: 1 cal publish per 5s
    last_cal_pub_ms = millis();
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_SYNC_OPS) return;
    telemetry_http_reset();
    vTaskDelay(pdMS_TO_TICKS(50));

    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    float stddev_out[8];
    uint8_t tick_count;
    sensor_get_baseline_progress(stddev_out, &tick_count);

    // Local doc — this function runs only from the network task and has
    // no other caller. Sharing with sync_device_channels_to_supabase()
    // (which uses g_rpc_doc) would require synchronising the two paths.
    StaticJsonDocument<512> cal_doc;
    cal_doc["device_key"] = device_key;
    cal_doc["calibrating"] = sensor_is_calibrating();
    cal_doc["baseline_tick"] = tick_count;
    JsonObject sd = cal_doc["baseline_stddev"].to<JsonObject>();
    char key[16];
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof(key), "ina3221_i%d", i);
        sd[key] = stddev_out[i];
    }
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof(key), "ina3221_v%d", i);
        sd[key] = stddev_out[i + 3];
    }
    cal_doc["updated_at"] = "now";

    static char buffer[512];
    size_t len = serializeJson(cal_doc, buffer);
    // Use security definer RPC to bypass RLS
    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/rpc/sync_sensor_calibration_status");
    int rc = supabase_post(path, buffer, len, supabase_url, anon_key);
    if (rc >= 200 && rc < 300) {
        LOG_PRINT("[CALIB] status synced: tick=%d\n", tick_count);
    } else {
        LOG_PRINT("[CALIB] sync FAILED: HTTP %d\n", rc);
    }
}

bool get_ble_pin_from_supabase(char* pin_str, size_t len) {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return false;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return false;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return false;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return false;

    // Use security definer RPC to bypass RLS
    JsonDocument rpc_doc;
    rpc_doc["p_device_key"] = device_key;
    static char buffer[256];
    size_t rpc_len = serializeJson(rpc_doc, buffer);

    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/rpc/get_device_ble_pin");
    supabase_http_reset();
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, path);
    if (!supabase_http_prepare(full_url, anon_key)) return false;
    int rc = g_supa_http.POST((uint8_t*)buffer, rpc_len);
    bool ok = false;
    if (rc == 200) {
        char body[256];
        size_t body_len = 0;
        Stream& stream = g_supa_http.getStream();
        unsigned long t0 = millis();
        while (stream.available() && body_len < sizeof(body)-1 && millis()-t0 < 2000) {
            int c = stream.read();
            if (c >= 0) body[body_len++] = (char)c;
        }
        body[body_len] = '\0';
        // RPC returns a quoted string, e.g. "\"1234\"" — strip quotes
        if (body_len > 2 && body[0] == '"') {
            size_t copy_len = body_len - 2;
            if (copy_len < len) {
                memcpy(pin_str, body + 1, copy_len);
                pin_str[copy_len] = '\0';
                ok = pin_str[0] != '\0';
            }
        }
    }
    supabase_http_reset();
    return ok;
}

void check_settings_commands() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PUBLISH) return;
    telemetry_http_reset();
    vTaskDelay(pdMS_TO_TICKS(50));

    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    static unsigned long last_check = 0;
    if (millis() - last_check < 5000) return;  // poll every 5s
    last_check = millis();

    // Local doc for the claim body — small, no need to share.
    StaticJsonDocument<256> claim_doc;
    claim_doc["p_device_key"] = device_key;
    static char buffer[256];
    size_t len = serializeJson(claim_doc, buffer);

    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s/rest/v1/rpc/claim_settings_command", supabase_url);
    // LOG_PRINT("[SETTINGS] claim URL: %s\n", full_url);
    // LOG_PRINT("[SETTINGS] claim body: %.*s\n", (int)len, buffer);
    // LOG_PRINT("[SETTINGS] claim header apikey: %s\n", anon_key);
    // LOG_PRINT("[SETTINGS] claim header Authorization: Bearer %s\n", anon_key);
    if (!supabase_http_prepare(full_url, anon_key)) return;
    int rc = g_supa_http.POST((uint8_t*)buffer, len);
    // Retry once on rc=-1 (TLS handshake failure after WiFi reconnect)
    if (rc < 0) {
        // LOG_PRINTLN("[SETTINGS] claim rc=-1, retrying...");
        supabase_http_reset();
        delay(100);
        if (!supabase_http_prepare(full_url, anon_key)) return;
        rc = g_supa_http.POST((uint8_t*)buffer, len);
    }
    // LOG_PRINT("[SETTINGS] claim HTTP rc=%d\n", rc);
    if (rc == 200 || rc == 201 || rc == 204) {
        // Heap-allocate the body buffer so this function's stack frame stays
        // under ~1 KB even on a 4 KB-stack task. 1.5 KB was the worst case.
        const size_t BODY_CAP = 2048;
        char* body = (char*)malloc(BODY_CAP);
        if (!body) { supabase_http_reset(); return; }
        size_t body_len = 0;
        Stream& stream = g_supa_http.getStream();
        unsigned long t0 = millis();
        while (stream.available() && body_len < BODY_CAP-1 && millis()-t0 < 2000) {
            int c = stream.read();
            if (c >= 0) body[body_len++] = (char)c;
        }
        body[body_len] = '\0';
        drain_response();

        // LOG_PRINT("[SETTINGS] claim body_len=%d body: %.*s\n", body_len, (int)body_len, body);
        if (body_len == 0) { supabase_http_reset(); free(body); return; }
        // Skip HTTP chunked encoding size prefix if present (e.g. "f2\r\n...")
        const char* json_start = body;
        if (body_len > 2 && body[0] != '{') {
            const char* newline = strstr(body, "\r\n");
            if (newline) {
                json_start = newline + 2;
                // LOG_PRINT("[SETTINGS] chunked prefix skipped, json_start at offset %d\n", json_start - body);
            }
        }
        LOG_PRINT("[SETTINGS] raw response: %.256s\n", body);
        if (json_start[0] == '\0' || strncmp(json_start, "null", 4) == 0) {
            supabase_http_reset();
            free(body);
            return;
        }

        // Heap-allocate the JSON parse buffer too — same reason. 2 KB is more
        // than enough for any reasonable claim_settings_command payload.
        const size_t RESP_CAP = 2048;
        char* resp_buf = (char*)malloc(RESP_CAP);
        if (!resp_buf) { supabase_http_reset(); free(body); return; }
        size_t json_offset = json_start - body;
        size_t json_len = body_len - json_offset;
        if (json_len > RESP_CAP - 1) json_len = RESP_CAP - 1;
        memcpy(resp_buf, json_start, json_len);
        resp_buf[json_len] = '\0';
        // LOG_PRINT("[SETTINGS] parse attempt: %.128s\n", resp_buf);

        // Local doc for the parsed response — this branch uses cmd_type +
        // payload only, and never re-enters the network task, so a stack-
        // allocated StaticJsonDocument is the right scope.
        StaticJsonDocument<1024> resp_doc;
        DeserializationError err = deserializeJson(resp_doc, resp_buf);
        if (err) {
            // LOG_PRINT("[SETTINGS] parse error: %s | body: %.200s\n", err.c_str(), resp_buf);
            supabase_http_reset();
            free(body);
            free(resp_buf);
            return;
        }

        const char* cmd_type = resp_doc["cmd_type"] | "";
        if (strlen(cmd_type) == 0) {
            supabase_http_reset();
            free(body);
            free(resp_buf);
            return;
        }

        // payload_buf was 1 KB on the stack — heap it. 1 KB still plenty.
        const size_t PAY_CAP = 1024;
        char* payload_buf = (char*)malloc(PAY_CAP);
        if (!payload_buf) {
            supabase_http_reset();
            free(body);
            free(resp_buf);
            return;
        }
        JsonVariant payload_var = resp_doc["payload"];
        if (payload_var.is<const char*>()) {
            strlcpy(payload_buf, payload_var.as<const char*>(), PAY_CAP);
        } else {
            serializeJson(payload_var, payload_buf, PAY_CAP);
        }
        // TODO: Supabase auth is the trust boundary; device_api_key verification
        // is a schema-side concern. The schema-fix agent will add a
        // device_api_key column and validation on claim_settings_command.
        if (apply_settings_command(cmd_type, payload_buf)) {
            apply_settings_posthook(cmd_type);
        } else {
            LOG_PRINT("[CMD] apply failed for %s — skipping posthook\n", cmd_type);
            free(payload_buf);
            supabase_http_reset();
            free(body);
            free(resp_buf);
            return;
        }
        g_deferred_requests |= 1;  // sync_device_channels
        if (strcmp(cmd_type, "set_relay") == 0) {
            uint8_t idx = 0;
            bool energize = false;
            if (JsonObject obj = resp_doc["payload"]) {
                idx = obj["idx"] | 0;
                LOG_PRINT("[SETTINGS] set_relay idx=%d has_is_energized=%d val=%d\n",
                    idx, !obj["is_energized"].isNull(), obj["is_energized"].as<int>());
                if (!obj["is_energized"].isNull()) {
                    energize = obj["is_energized"].as<bool>();
                    switch_set(idx, energize);  // toggles GPIO + publishes to Supabase
                    g_deferred_requests &= ~4; // skip deferred sync (switch_set already published)
                } else {
                    g_deferred_relay_idx = idx;
                    g_deferred_relay_state = get_switch_state(idx);
                    g_deferred_requests |= 4;  // sync switch state via deferred path
                }
            }
        }
        // Done with the heap-allocated read buffers — release before next tick.
        supabase_http_reset();
        free(body);
        free(resp_buf);
        free(payload_buf);
    } else {
        // Read any error body before resetting — stale response data corrupts subsequent requests.
        // Heap-allocate the 256 B so it doesn't compound with the 1.5 KB+ in the success path.
        char* err_body = (char*)malloc(256);
        if (err_body) {
            size_t err_len = 0;
            Stream& err_stream = g_supa_http.getStream();
            unsigned long t0 = millis();
            while (err_stream.available() && err_len < 255 && millis()-t0 < 1000) {
                int c = err_stream.read();
                if (c >= 0) err_body[err_len++] = (char)c;
            }
            err_body[err_len] = '\0';
            free(err_body);
        }
        drain_response();
        supabase_http_reset();
        static int settings_fail_count = 0;
        // LOG_PRINT("[SETTINGS] claim failed rc=%d fail_count=%d err_body(%d): %.*s\n", ...);
        // LOG_PRINT("[SETTINGS] claim failed rc=%d fail_count=%d err_body: (empty)\n", ...);
        if (++settings_fail_count >= 3) {
            settings_fail_count = 0;
        }
    }
}
