#include "connectivity_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "log_serial.h"
#include "settings_manager.h"
#include "data_logger.h"
#include "ble_provisioner.h"
#include "event_log.h"
#include "device_identity.h"
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
#if USE_PROTOBUF
#include "telemetry_pb.h"
#endif
#include <HTTPClient.h>
#include <time.h>
#include <stdlib.h>     // setenv
#include <math.h>       // isfinite
#include <esp_sntp.h>

static WiFiClientSecure mqttClient;
static PubSubClient mqtt(mqttClient);

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
        // 10 s handshake (was 30) keeps any single TLS connect well under the
        // 30 s task WDT; a dead server fails faster instead of stalling the
        // network task into a WDT reboot loop.
        g_telemetry_client.setHandshakeTimeout(10);
        g_telemetry_http.setReuse(true);  // connection reuse for high-frequency telemetry
        g_telemetry_http.setTimeout(4000);  // bound each request well under the WDT
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
        g_supa_client.setHandshakeTimeout(10);  // 10 s (was 30) — see telemetry_http_prepare
        g_supa_client_configured = true;
    }
    g_supa_http.setReuse(false);
    g_supa_http.setTimeout(4000);  // bound each low-freq request under the WDT
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

// ── New backend (Go API) command-queue HTTP helpers ────────────────────────
// The new backend authenticates firmware requests with X-Device-Key /
// X-Api-Key headers (see backend/internal/middleware.go DeviceAuthMiddleware),
// not the Supabase apikey/Authorization pair. Reuses the g_supa_http /
// g_supa_client connection-teardown machinery above (already the right
// pattern for low-frequency polls) with a different header set.
static bool backend_http_prepare(const char* full_url, const char* device_key, const char* api_key) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_LOWFREQ) return false;
    if (g_supa_http_ready) {
        supabase_http_reset();
    }
    static bool g_supa_client_configured = false;
    if (!g_supa_client_configured) {
        g_supa_client.setInsecure();
        g_supa_client.setHandshakeTimeout(10);
        g_supa_client_configured = true;
    }
    g_supa_http.setReuse(false);
    g_supa_http.setTimeout(4000);
    if (!g_supa_http.begin(g_supa_client, full_url)) {
        LOG_PRINT("[BACKEND_HTTP] begin failed: %s\n", full_url);
        supabase_http_reset();
        return false;
    }
    g_supa_http.addHeader("Content-Type", "application/json");
    g_supa_http.addHeader("X-Device-Key", device_key);
    g_supa_http.addHeader("X-Api-Key", api_key);
    g_supa_http_ready = true;
    return true;
}

static int backend_get(const char* url_path, const char* backend_url,
                        const char* device_key, const char* api_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", backend_url, url_path);
    if (!backend_http_prepare(full_url, device_key, api_key)) return -1;
    int rc = g_supa_http.GET();
    if (rc < 0) supabase_http_reset();
    return rc;
}

static int backend_post(const char* url_path, const char* payload, size_t len,
                         const char* backend_url, const char* device_key, const char* api_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", backend_url, url_path);
    if (!backend_http_prepare(full_url, device_key, api_key)) return -1;
    int rc = g_supa_http.POST((uint8_t*)payload, len);
    if (rc < 0) {
        supabase_http_reset();
        delay(50);
        if (!backend_http_prepare(full_url, device_key, api_key)) return -1;
        rc = g_supa_http.POST((uint8_t*)payload, len);
    }
    drain_response();
    supabase_http_reset();
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
bool mqtt_is_connected() { return mqtt.connected(); }
bool network_is_skipped() { return skip_network; }

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
        log_event(EVENT_LOG_WARN, "ntp", "sync timeout");
        return SYNC_TIMEOUT;
    }
    epoch_time = t;
    log_set_epoch(t);
    log_epoch_valid_set(true);  // trusted post-2023 epoch
    g_ntp_synced = true;
    log_event(EVENT_LOG_INFO, "ntp", "synced");
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

// Post-connect work is split across ticks so each blocking call (NTP sync,
// Supabase calibration sync, MQTT connect) runs in its own 10 ms network-task
// tick instead of all at once in the CONNECTING->CONNECTED transition. This
// keeps every tick well under the 30 s WDT and lets mqtt.loop()/BLE run between
// steps. Steps run in order; PCS_DONE means steady state.
enum PostConnectStep { PCS_IDLE, PCS_NTP, PCS_CAL, PCS_MQTT, PCS_DONE };
static PostConnectStep s_post_connect = PCS_IDLE;
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
                log_event(EVENT_LOG_INFO, "wifi", "connected %s", ip_str);
                // Disable BLE to free ~50KB heap for TLS operations.
                LOG_PRINTLN("[BLE] disabling BLE stack to free heap for TLS");
                deinit_ble_provisioner();
                // Defer NTP / Supabase calibration sync / MQTT connect to the
                // WST_CONNECTED tick (s_post_connect) so no single tick does
                // all of it and risks the 30 s WDT.
                s_post_connect = PCS_NTP;
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
            // Run post-connect work one step per tick so each blocking call
            // (NTP, Supabase calibration sync, MQTT connect) is isolated and
            // the task can still service mqtt.loop()/BLE between steps.
            if (s_post_connect == PCS_NTP) {
                SyncResult r = sync_time();
                s_post_connect = (r == SYNC_OK) ? PCS_CAL : PCS_MQTT;
                return;
            }
            if (s_post_connect == PCS_CAL) {
                sync_calibration_to_supabase();
                s_post_connect = PCS_MQTT;
                return;
            }
            if (s_post_connect == PCS_MQTT) {
                char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
                if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
                    mqtt.setServer(mqtt_broker, mqtt_port);
                    connect_mqtt();
                } else {
                    LOG_PRINTLN("MQTT: not configured (skip)");
                }
                s_post_connect = PCS_DONE;
                return;
            }
            // Steady state (PCS_IDLE/PCS_DONE). Disconnect detection happens
            // in loop_connectivity.
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

// Resolve the per-device MQTT identity. The backend authenticates each device
// with device_key (username/client id) + api_key (password), publishes
// status/{device_key}/online for presence, and subscribes to
// telemetry/{device_type}/{device_key} for ingestion. If no device_key is
// configured (e.g. dev broker, no Supabase provisioning), fall back to a
// MAC-derived id so two devices don't collide on a shared client id.
static void get_mqtt_identity(char* device_key, size_t key_len) {
    if (key_len > 0) device_key[0] = '\0';
    char dk[64] = "";
    if (settings_load_supabase_device_key(dk, sizeof(dk)) && is_valid_uuid(dk)) {
        strlcpy(device_key, dk, key_len);
        return;
    }
    // Fallback: MAC-based unique id (no backend auth; for dev/anonymous broker).
    snprintf(device_key, key_len, "dev-%s", WiFi.macAddress().c_str());
}

static void publish_online_status(const char* device_key, bool online) {
    char topic[96];
    snprintf(topic, sizeof(topic), "status/%s/online", device_key);
    // Backend expects {"online":bool,"ts":unix_seconds}; ts=0 falls back to
    // the server clock, but send epoch when NTP has synced. The schema/fw
    // fields let the backend negotiate/record the device's telemetry shape
    // (forward-looking: a future backend can reject or upgrade on mismatch).
    char payload[96];
    time_t ts = get_epoch_time();
    snprintf(payload, sizeof(payload),
             "{\"online\":%s,\"ts\":%ld,\"schema\":\"%s\",\"fw\":\"%s\"}",
             online ? "true" : "false", (long)ts,
             TELEMETRY_PROFILE_STRING, TELEMETRY_FW_VERSION);
    // Retained so a freshly-subscribed backend sees the last known state.
    mqtt.publish(topic, payload, true);
}

static void connect_mqtt() {
    if (skip_network) return;
    static uint32_t last_mqtt_retry = -30000UL; // underflow so first call passes
    if (millis() - last_mqtt_retry < 30000) return; // rate limit: 1 attempt per 30s
    last_mqtt_retry = millis();

    // Configure TLS for MQTT. Using setInsecure() accepts any server cert;
    // for production with a known broker, replace with setCACert().
    mqttClient.setInsecure();
    mqttClient.setHandshakeTimeout(10);  // 10 s handshake timeout

    char device_key[64];
    get_mqtt_identity(device_key, sizeof(device_key));

    // LWT: broker publishes {"online":false} when this client drops ungracefully.
    // QoS 1 + retained so the backend's online/offline detection is reliable.
    char will_topic[96];
    snprintf(will_topic, sizeof(will_topic), "status/%s/online", device_key);
    char will_payload[48];
    snprintf(will_payload, sizeof(will_payload), "{\"online\":false,\"ts\":0}");

    // Authenticate with device_key/api_key when both are configured (production
    // backend requires it). Without them, connect anonymously (dev broker).
    char api_key[128] = "";
    bool have_auth = settings_load_supabase_api_key(api_key, sizeof(api_key)) && api_key[0];
    bool connected;
    if (have_auth) {
        connected = mqtt.connect(device_key, device_key, api_key,
                                  will_topic, 1, true, will_payload);
    } else {
        connected = mqtt.connect(device_key, will_topic, 1, true, will_payload);
    }

    if (connected) {
        LOG_PRINT("MQTT connected (client=%s, auth=%s)\n", device_key, have_auth ? "yes" : "no");
        log_event(EVENT_LOG_INFO, "mqtt", "connected");
        // Override the LWT with our actual liveness state now that we're up.
        publish_online_status(device_key, true);
    } else {
        LOG_PRINT("MQTT fail rc=%d\n", mqtt.state());
        log_event(EVENT_LOG_WARN, "mqtt", "connect failed rc=%d", mqtt.state());
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
    http.setTimeout(4000);  // bound each request under the 30 s task WDT
    // http.begin(url) (single-arg) cannot do TLS on ESP32 Arduino; an https
    // URL must be handed to begin(WiFiClientSecure&, url). Use a secure client
    // for https:// and the plain TCP client for http://.
    bool is_https = (strncmp(url, "https://", 8) == 0);
    bool begun;
    if (is_https) {
        static WiFiClientSecure https_client;  // reused across calls; setInsecure once
        https_client.setInsecure();
        https_client.setHandshakeTimeout(10);
        begun = http.begin(https_client, url);
    } else {
        begun = http.begin(url);
    }
    if (!begun) {
        LOG_PRINTLN("[HTTP] begin() failed");
        return;
    }
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

    // WiFi dropped — bring BLE back up so the device can be re-provisioned.
    // WiFi connect tears the NimBLE stack down (deinit_ble_provisioner, to free
    // ~50KB heap for TLS), so a bare start_ble_advertising() here does nothing —
    // ble_initialized is false and it early-returns. Re-create the stack via
    // init_ble_provisioner(); it's idempotent (gated on ble_initialized) so the
    // per-tick calls while disconnected are cheap no-ops after the first.
    if (wifi_was_connected && !wifi_connected) {
        LOG_PRINTLN("[WiFi] disconnected — re-enabling BLE provisioning");
        log_event(EVENT_LOG_WARN, "wifi", "disconnected");
        init_ble_provisioner();
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

    // Pending batch from a previous failed publish — emit it before popping
    // new data so a transient broker failure doesn't lose the popped entries.
    static uint8_t pending[512];
    static size_t pending_len = 0;

    if (pending_len > 0) {
        base64_encode(pending, pending_len, encoded);
        if (mqtt.publish(MQTT_LOG_TOPIC, encoded)) {
            pending_len = 0;
        } else {
            return;  // still can't publish; keep pending for next tick
        }
    }

    // Drain RAM buffer first. Only pop one batch at a time: if the publish
    // fails, stash it in `pending` and stop so it is retried next tick instead
    // of being dropped (the old loop popped everything up front and lost the
    // tail on the first failed publish).
    size_t batch_len = log_pop_batch(batch, sizeof(batch));
    if (batch_len > 0) {
        base64_encode(batch, batch_len, encoded);
        if (mqtt.publish(MQTT_LOG_TOPIC, encoded)) {
            // success — keep draining in subsequent calls
        } else {
            memcpy(pending, batch, batch_len);
            pending_len = batch_len;
#if CORE_DEBUG_LEVEL >= 3
            LOG_PRINTLN("[MQTT] log publish() returned false — batch queued for retry");
#endif
            return;
        }
    }

    // Then drain LittleFS overflow file. Only DELETE the file once it has
    // been fully drained (n == 0); on a mid-drain publish failure, keep the
    // file so the remaining entries survive for the next attempt.
    if (log_open_overflow_for_read()) {
        bool drained = true;
        while (true) {
            size_t n = log_read_overflow_chunk(batch, sizeof(batch));
            if (n == 0) break;
            base64_encode(batch, n, encoded);
            if (!mqtt.publish(MQTT_LOG_TOPIC, encoded)) {
#if CORE_DEBUG_LEVEL >= 3
                LOG_PRINTLN("[MQTT] log publish() returned false during overflow drain");
#endif
                drained = false;
                break;
            }
        }
        if (drained) {
            log_close_overflow();       // fully drained: delete the file
        } else {
            log_close_overflow_keep();  // partial drain: keep the file for retry
        }
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
    // Flat format matching backend ingest.go:
    //   ts, ts_ms, schema, fw, uptime_ms, rssi, heap_free at top level
    //   data: map[string]float64 with all channel/switch/battery/log/health values
    root["ts"] = s.ts;
    root["ts_ms"] = s.ts_ms;
    root["schema"] = s.schema;
    root["fw"] = s.device.fw;
    root["uptime_ms"] = s.device.uptime_ms;
    root["rssi"] = s.wifi.rssi;
    root["heap_free"] = s.heap_free;
    root["hw_rev"] = s.hw_rev;

    // time_source: "ntp" when NTP has synced and ts is valid, else "uptime"
    if (s.ts == 0 || !g_ntp_synced) {
        root["time_source"] = "uptime";
    } else {
        root["time_source"] = "ntp";
    }

    JsonObject data = root["data"].to<JsonObject>();

    // ── Channels ──────────────────────────────────────────────────────────
    for (uint8_t i = 0; i < s.channel_count; i++) {
        const TelemetryChannel& c = s.channels[i];
        char key[24];
        snprintf(key, sizeof(key), "ch%u_V", i);
        write_float(data, key, c.V);
        snprintf(key, sizeof(key), "ch%u_I", i);
        write_float(data, key, c.I);
        snprintf(key, sizeof(key), "ch%u_P", i);
        write_float(data, key, c.P);
        snprintf(key, sizeof(key), "ch%u_energy_Wh", i);
        write_float(data, key, c.energy_Wh);
        snprintf(key, sizeof(key), "ch%u_charge_mAh", i);
        write_float(data, key, c.charge_mAh);
    }

    // ── Switches ──────────────────────────────────────────────────────────
    for (uint8_t i = 0; i < s.switch_count; i++) {
        const TelemetrySwitch& sw = s.switches[i];
        char key[24];
        snprintf(key, sizeof(key), "sw%u_state", i);
        data[key] = sw.state ? 1.0f : 0.0f;
        snprintf(key, sizeof(key), "sw%u_type", i);
        data[key] = sw.type;
        snprintf(key, sizeof(key), "sw%u_auto", i);
        data[key] = sw.auto_mode ? 1.0f : 0.0f;
        snprintf(key, sizeof(key), "sw%u_rule_tripped", i);
        data[key] = sw.rule_tripped ? 1.0f : 0.0f;
    }

    // ── Batteries ─────────────────────────────────────────────────────────
    for (uint8_t i = 0; i < s.battery_count; i++) {
        const TelemetryBattery& b = s.battery[i];
        char key[32];
        snprintf(key, sizeof(key), "bat%u_soc_pct", i);
        write_float(data, key, b.soc_pct);
        snprintf(key, sizeof(key), "bat%u_V", i);
        write_float(data, key, b.V);
        snprintf(key, sizeof(key), "bat%u_I", i);
        write_float(data, key, b.I);
        snprintf(key, sizeof(key), "bat%u_cumulative_Ah_in", i);
        write_float(data, key, b.cumulative_Ah_in);
        snprintf(key, sizeof(key), "bat%u_cumulative_Ah_out", i);
        write_float(data, key, b.cumulative_Ah_out);
        snprintf(key, sizeof(key), "bat%u_equivalent_full_cycles", i);
        write_float(data, key, b.equivalent_full_cycles);
        snprintf(key, sizeof(key), "bat%u_soh_pct", i);
        write_float(data, key, b.soh_pct);
        snprintf(key, sizeof(key), "bat%u_soh_samples", i);
        data[key] = b.soh_samples;
    }

    // ── Log metadata ─────────────────────────────────────────────────────
    data["log_entries"] = s.log.entries;
    data["log_overflow"] = s.log.overflow ? 1.0f : 0.0f;

    // ── System health ─────────────────────────────────────────────────────
    data["min_free_heap"] = (double)s.min_free_heap;
    data["reset_reason"] = s.reset_reason;
    data["crash_count"] = (double)s.crash_count;
    data["safe_mode"] = s.safe_mode ? 1.0f : 0.0f;
    data["ntp_synced"] = s.ntp_synced ? 1.0f : 0.0f;
    data["ble_active"] = s.ble_active ? 1.0f : 0.0f;
    data["ble_connected"] = s.ble_connected ? 1.0f : 0.0f;
    data["mqtt_connected"] = s.mqtt_connected ? 1.0f : 0.0f;
    data["http_configured"] = s.http_configured ? 1.0f : 0.0f;
    data["network_skipped"] = s.network_skipped ? 1.0f : 0.0f;
    data["sd_present"] = s.sd_present ? 1.0f : 0.0f;
    data["log_buffer_used_pct"] = s.log_buffer_used_pct;
    data["sensors_calibrating"] = s.sensors_calibrating ? 1.0f : 0.0f;

    // ── OTA status (numeric subset) ───────────────────────────────────────
    data["ota_in_progress"] = s.ota.ota_in_progress ? 1.0f : 0.0f;
    data["ota_progress_pct"] = s.ota.ota_progress_pct;
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

static JsonDocument g_pub_doc;

// Drain one pending telemetry overflow entry and publish it via MQTT.
// Called at the start of publish_data() so queued payloads are retried
// before new data is published. Drains at most one entry per call to
// keep the tick short.
static void drain_pending_telemetry_overflow() {
    if (!sd_is_present()) return;
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (!settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) return;
    if (!mqtt.connected()) return;

    char payload[2048];
    size_t plen = drain_telemetry_overflow(payload, sizeof(payload));
    if (plen == 0) return;

    char device_key[64];
    get_mqtt_identity(device_key, sizeof(device_key));
    char topic[128];
    snprintf(topic, sizeof(topic), "telemetry/%s/%s", DEVICE_TYPE, device_key);
    if (mqtt.publish(topic, payload)) {
        LOG_PRINT("[MQTT] overflow replay: %u bytes\n", (unsigned)plen);
    } else {
        // Re-queue on failure — prepend back to the file
        save_telemetry_overflow(payload, plen);
    }
}

void publish_data(const SensorSnapshot& data) {
    if (skip_network) return;
    // Build once and delegate so the MQTT/HTTP/BLE path and the Supabase path
    // share a single TelemetrySnapshot — see publish_data(data, snap).
    TelemetrySnapshot snap;
    telemetry_build(snap);
    publish_data(data, snap);
}

void publish_data(const SensorSnapshot& data, const TelemetrySnapshot& snap) {
    (void)data;
    if (skip_network) return;

    // Drain any pending telemetry overflow entries before publishing new
    // data. This retries failed publishes from previous ticks.
    drain_pending_telemetry_overflow();

    // snap was built by the caller (publish_data(data)) so both transports
    // share one TelemetrySnapshot. telemetry_build() clears the one-shot
    // capacity-test SoH flag, so building twice per cycle (the old behavior)
    // meant the second publish never carried capacity_test_soh_valid.
    // New shape: serialize TelemetrySnapshot into a 4 KB buffer.
    static char buffer[TELEMETRY_BUF_BYTES];
    size_t len = serialize_telemetry(snap, buffer, sizeof(buffer));
    if (len == 0) {
        // Serialization overflowed the 4 KB buffer (or the snapshot is huge).
        // Previously this returned silently, so a swollen payload quietly
        // blackholed telemetry with no log trail. Log it so the operator can
        // raise TELEMETRY_BUF_BYTES or trim the schema.
        LOG_PRINTLN("[TELEM] serialize overflow — publish dropped (raise TELEMETRY_BUF_BYTES)");
        return;
    }
#if CORE_DEBUG_LEVEL >= 3
    LOG_PRINT("[TELEM] %u bytes (ch=%u sw=%u bat=%u heap=%u)\n",
        (unsigned)len, snap.channel_count, snap.switch_count, snap.battery_count,
        ESP.getFreeHeap());
#endif

    // MQTT — publish to the backend contract topic
    // telemetry/{device_type}/{device_key}. Keep the 1-second rate limit so
    // the broker doesn't drown.
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic_unused[64];
    static unsigned long last_mqtt_pub = 0;
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic_unused, sizeof(mqtt_broker))) {
        if (mqtt.connected() && millis() - last_mqtt_pub >= 1000) {
            last_mqtt_pub = millis();
            char device_key[64];
            get_mqtt_identity(device_key, sizeof(device_key));
            char topic[128];
#if USE_PROTOBUF
            // Protobuf-encode and publish to the /pb topic suffix. The backend
            // does not yet consume this topic — this is firmware-side plumbing
            // only. JSON publish is skipped when protobuf is active.
            static uint8_t pb_buf[TELEMETRY_BUF_BYTES];
            size_t pb_len = encode_telemetry_pb(snap, pb_buf, sizeof(pb_buf));
            if (pb_len > 0) {
                snprintf(topic, sizeof(topic), "telemetry/%s/%s/pb", DEVICE_TYPE, device_key);
                if (!mqtt.publish(topic, (const char*)pb_buf, pb_len)) {
                    LOG_PRINTLN("[MQTT] pb publish() returned false");
                }
            } else {
                LOG_PRINTLN("[MQTT] pb encode failed — skipping publish");
            }
#else
            snprintf(topic, sizeof(topic), "telemetry/%s/%s", DEVICE_TYPE, device_key);
            if (!mqtt.publish(topic, buffer, len)) {
                LOG_PRINTLN("[MQTT] publish failed — queuing to SD overflow");
                save_telemetry_overflow(buffer, len);
            }
#endif
            // Periodic presence heartbeat: the backend's staleness sweep marks
            // a device offline after 60 s without an online message, so
            // re-publish every 45 s while connected.
            static unsigned long last_heartbeat = 0;
            if (millis() - last_heartbeat >= 45000) {
                last_heartbeat = millis();
                publish_online_status(device_key, true);
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
        // Subset serializer overflowed. The full buffer (up to ~1.4 KB) does
        // NOT fit the default 20-byte ATT MTU, so sending it would truncate
        // mid-notify and hand the dashboard a corrupt JSON fragment. Drop the
        // notify instead — the next tick's subset will go out once the
        // payload shrinks (it's bounded at full channel saturation).
        LOG_PRINTLN("[BLE] telemetry subset overflow — notify dropped (MTU too small for full payload)");
    }
    vTaskDelay(pdMS_TO_TICKS(25));  // space out notifies — avoids BLE stack crowding / UX jitter

    // Legacy Supabase side syncs with no backend-native replacement yet:
    // battery-profile heartbeat and live calibration-progress. Both used to
    // run inside the now-removed publish_data_supabase(); keep them firing
    // for any device that still has Supabase configured so the dashboard
    // doesn't lose these two views. publish_calibration_status() loads its
    // own credentials and no-ops when nothing is configured or no
    // calibration is active.
    char supabase_url[128], supabase_anon_key[128];
    if (settings_load_supabase_url(supabase_url, sizeof(supabase_url)) &&
        settings_load_supabase_anon_key(supabase_anon_key, sizeof(supabase_anon_key))) {
        publish_battery_profiles_heartbeat(supabase_url, supabase_anon_key);
    }
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

void publish_ota_status(const char* status, const char* version,
                         uint8_t progress_pct, const char* error) {
    if (!mqtt.connected()) return;

    char topic[128];
    char device_key[64] = "";
    settings_load_supabase_device_key(device_key, sizeof(device_key));
    if (device_key[0] == '\0') {
        // Fall back to device serial if no device_key configured
        strncpy(device_key, get_device_serial(), sizeof(device_key) - 1);
    }
    snprintf(topic, sizeof(topic), "status/%s/ota", device_key);

    StaticJsonDocument<256> doc;
    doc["status"] = status;
    if (version && version[0]) doc["version"] = version;
    if (progress_pct > 0) doc["progress"] = progress_pct;
    if (error && error[0]) doc["error"] = error;

    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n > 0) {
        mqtt.publish(topic, 0, false, buf);
    }
}

void apply_settings_posthook(const char* cmd_type) {
    if (strcmp(cmd_type, "set_wifi") == 0) {
        char ssid[64] = "", pass[64] = "";
        if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
            WiFi.disconnect(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            WiFi.begin(ssid, pass);
            // Reset the connection state machine so the post-connect sequence
            // (NTP sync, Supabase calibration sync, MQTT connect) re-runs for
            // the new link. Without this, s_wifi_state stays WST_CONNECTED and
            // s_post_connect stays PCS_DONE, so a credentials change would
            // leave NTP/MQTT bound to the old (or no) session.
            s_post_connect = PCS_IDLE;
            wifi_state_enter(WST_CONNECTING);
            s_wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
            g_ntp_synced = false;
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

    static unsigned long last_check = 0;
    if (millis() - last_check < 5000) return;  // poll every 5s
    last_check = millis();

    char backend_url[128], device_key[64], api_key[64];
    if (!settings_load_ota_backend_url(backend_url, sizeof(backend_url))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    char path[128];
    snprintf(path, sizeof(path), "/commands/%s/pending", device_key);
    int rc = backend_get(path, backend_url, device_key, api_key);
    if (rc != 200) {
        if (rc > 0) {
            LOG_PRINT("[CMD] pending-poll failed: HTTP %d\n", rc);
        }
        supabase_http_reset();
        return;
    }

    // Heap-allocate the response body — keeps this function's stack frame
    // small on the 4 KB network-task stack (same reasoning as the old
    // Supabase claim path this replaces).
    const size_t BODY_CAP = 2048;
    char* body = (char*)malloc(BODY_CAP);
    if (!body) { supabase_http_reset(); return; }
    size_t body_len = 0;
    Stream& stream = g_supa_http.getStream();
    unsigned long t0 = millis();
    while (stream.available() && body_len < BODY_CAP - 1 && millis() - t0 < 2000) {
        int c = stream.read();
        if (c >= 0) body[body_len++] = (char)c;
    }
    body[body_len] = '\0';
    supabase_http_reset();

    if (body_len == 0) { free(body); return; }

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, body);
    free(body);
    if (err) {
        LOG_PRINT("[CMD] pending-poll JSON parse error: %s\n", err.c_str());
        return;
    }

    JsonArray commands = doc.as<JsonArray>();
    if (commands.isNull()) return;

    // Bound the work done per poll tick — a burst of queued commands should
    // drain over a few ticks rather than blocking the network task for one
    // long tick (mirrors the 5-entries-per-call pattern used elsewhere in
    // this file for log draining).
    uint8_t processed = 0;
    for (JsonObject cmd : commands) {
        if (++processed > 4) break;

        long long cmd_id = cmd["id"] | 0LL;
        const char* cmd_type = cmd["cmd_type"] | "";
        if (cmd_id == 0 || strlen(cmd_type) == 0) continue;

        const size_t PAY_CAP = 1024;
        char* payload_buf = (char*)malloc(PAY_CAP);
        if (!payload_buf) continue;
        JsonVariant payload_var = cmd["payload"];
        if (payload_var.is<const char*>()) {
            strlcpy(payload_buf, payload_var.as<const char*>(), PAY_CAP);
        } else {
            serializeJson(payload_var, payload_buf, PAY_CAP);
        }

        bool applied = apply_settings_command(cmd_type, payload_buf);
        if (applied) {
            apply_settings_posthook(cmd_type);
            g_deferred_requests |= 1;  // sync_device_channels

            // Immediate relay energize/de-energize: set_relay carries
            // is_energized directly (distinct from the rule-config fields
            // apply_settings_command already persisted above).
            if (strcmp(cmd_type, "set_relay") == 0) {
                if (JsonObject obj = payload_var.as<JsonObject>()) {
                    uint8_t idx = obj["idx"] | 0;
                    if (!obj["is_energized"].isNull()) {
                        bool energize = obj["is_energized"].as<bool>();
                        switch_set(idx, energize);  // toggles GPIO + publishes state
                        g_deferred_requests &= ~4;  // switch_set already published
                    } else {
                        g_deferred_relay_idx = idx;
                        g_deferred_relay_state = get_switch_state(idx);
                        g_deferred_requests |= 4;  // sync switch state via deferred path
                    }
                }
            }
        } else {
            LOG_PRINT("[CMD] apply failed for %s\n", cmd_type);
        }
        free(payload_buf);

        // Report the outcome back to the backend so the web UI's command
        // history reflects reality (the old Supabase path never reported
        // results — this is new, backend-required behavior).
        StaticJsonDocument<256> result_doc;
        result_doc["status"] = applied ? "applied" : "failed";
        if (!applied) result_doc["error"] = "apply_settings_command failed";
        char result_buf[256];
        size_t result_len = serializeJson(result_doc, result_buf);
        char result_path[64];
        snprintf(result_path, sizeof(result_path), "/commands/%lld/result", cmd_id);
        int result_rc = backend_post(result_path, result_buf, result_len, backend_url, device_key, api_key);
        if (result_rc < 200 || result_rc >= 300) {
            LOG_PRINT("[CMD] result report failed for id=%lld: HTTP %d\n", cmd_id, result_rc);
        }
    }
}
