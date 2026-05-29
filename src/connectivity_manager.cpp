#include "connectivity_manager.h"
#include "config.h"
#include "settings_manager.h"
#include "data_logger.h"
#include "ble_provisioner.h"
#include "coulomb_counter.h"
#include "energy_counter.h"
#include "sensor_manager.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#define MQTT_MAX_PACKET_SIZE 1024
#include <PubSubClient.h>
#include <ArduinoJson.h>
// #include <BlynkSimpleEsp32.h> // Blynk disabled
// Blynk disabled — uncomment above and set BLYNK_AUTH_TOKEN to enable
#include <HTTPClient.h>
#include <time.h>

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

// Persistent HTTPS client for Supabase — avoids repeated TLS handshakes / heap churn
static WiFiClientSecure g_supa_client;
static HTTPClient       g_supa_http;
static bool             g_supa_http_ready = false;

static void supabase_http_reset() {
    if (g_supa_http_ready) {
        g_supa_http.end();
        g_supa_http_ready = false;
    }
}

static bool supabase_http_prepare(const char* full_url, const char* anon_key) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!g_supa_http_ready) {
        g_supa_client.setInsecure(); // skip cert verification
        g_supa_http.setReuse(true);
        if (!g_supa_http.begin(g_supa_client, full_url)) {
            return false;
        }
        g_supa_http.addHeader("Content-Type", "application/json");
        g_supa_http.addHeader("apikey", anon_key);
        char auth_hdr[384];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", anon_key);
        g_supa_http.addHeader("Authorization", auth_hdr);
        g_supa_http_ready = true;
    } else {
        g_supa_http.begin(g_supa_client, full_url); // same host → keeps connection alive
    }
    return true;
}

static int supabase_post(const char* url_path, const char* payload, size_t len,
    const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!supabase_http_prepare(full_url, anon_key)) return -1;
    int rc = g_supa_http.POST((uint8_t*)payload, len);
    if (rc < 0) supabase_http_reset();
    return rc;
}

static int supabase_patch(const char* url_path, const char* payload, size_t len,
    const char* supabase_url, const char* anon_key) {
    char full_url[256];
    snprintf(full_url, sizeof(full_url), "%s%s", supabase_url, url_path);
    if (!supabase_http_prepare(full_url, anon_key)) return -1;
    int rc = g_supa_http.sendRequest("PATCH", (uint8_t*)payload, len);
    if (rc < 0) supabase_http_reset();
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
float get_sensor_voltage(uint8_t src, uint8_t idx, const SensorData& data) {
    if (src == 1) return data.ads1115_volts[idx < 4 ? idx : 0];        // INA3221 voltage module
    if (src == 2) return data.ina3221_busV[idx < 3 ? idx : 0];        // INA3221 current module (busV)
    if (src == 3) return data.ina226_busV;                            // INA226
    if (src == 4) return data.ads1115_volts[idx < 4 ? idx : 0];      // ADS1115
    return 0.0f;
}
float get_sensor_current(uint8_t src, uint8_t idx, const SensorData& data) {
    if (src == 1) return 0.0f;                                       // voltage-only source
    if (src == 2) return data.ina3221_current[idx < 3 ? idx : 0];    // INA3221 current module
    if (src == 3) return data.ina226_current;                         // INA226
    return 0.0f;
}
float get_sensor_power(uint8_t src, uint8_t idx, const SensorData& data) {
    if (src == 3) return data.ina226_power;                          // INA226 has built-in power
    return 0.0f;
}

static void sync_time() {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    struct tm ti = {};
    if (getLocalTime(&ti, 5000)) {
        epoch_time = mktime(&ti);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        Serial.print("NTP time: "); Serial.println(buf);
    } else {
        Serial.println("NTP sync failed");
    }
}

static void connect_wifi() {
    char ssid[64], pass[64];
    if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
        WiFi.begin(ssid, pass);
    } else {
        // Use compile-time defaults only if they look real (not placeholder)
        if (strlen(WIFI_SSID) > 5 && strcmp(WIFI_SSID, "YOUR_SSID") != 0) {
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        } else {
            Serial.println("WiFi: no credentials — offline mode");
            skip_network = true;
            return;
        }
    }
    int attempts = 20; // ~10 seconds timeout
    while (WiFi.status() != WL_CONNECTED && attempts-- > 0) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi: connection failed — offline mode");
        skip_network = true;
        return;
    }
    IPAddress ip = WiFi.localIP();
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    Serial.println("\nWiFi connected");
    sync_time();
    sync_calibration_to_supabase();
}

static void connect_mqtt() {
    if (skip_network) return;
    while (!mqtt.connected()) {
        if (mqtt.connect("power-monitor-esp32")) {
            Serial.println("MQTT connected");
        } else {
            Serial.print("MQTT fail rc=");
            Serial.println(mqtt.state());
            delay(5000);
        }
    }
}

const char* get_local_ip_str() { return ip_str; }
time_t get_epoch_time() { return epoch_time; }

static void print_http_error(HTTPClient& http, int rc) {
    Serial.printf("HTTP error %d (heap=%u)\n", rc, ESP.getFreeHeap());
    if (rc < 0) return; // no body on connection-level failure
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) return;
    char body[512];
    int n = 0;
    unsigned long t0 = millis();
    while (stream->available() && n < (int)sizeof(body) - 1 && millis() - t0 < 300) {
        body[n++] = stream->read();
    }
    body[n] = '\0';
    if (n > 0) {
        Serial.print("Response body: ");
        Serial.println(body);
    }
}

void publish_data_http(const SensorData& data, const char* json_buffer, size_t json_len) {
    if (ESP.getFreeHeap() < 4096) return;
    if (!settings_load_http_enabled()) return;
    char url[128], token[64];
    if (!settings_load_http_endpoint(url, token, sizeof(url))) return;
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
    connect_wifi();

    if (skip_network) {
        Serial.println("Network: offline mode active");
        return;
    }

    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        mqtt.setServer(mqtt_broker, mqtt_port);
        connect_mqtt();
    } else {
        Serial.println("MQTT: not configured (skip)");
    }

    // Blynk disabled — uncomment lib_deps in platformio.ini and set BLYNK_AUTH_TOKEN to enable
    // if (strcmp(BLYNK_AUTH_TOKEN, "YOUR_BLYNK_TOKEN") != 0) { ... }
}

void loop_connectivity() {
    static bool wifi_was_connected = false;
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);

    // WiFi reconnection — attempt every 30s when disconnected
    static uint32_t last_wifi_retry = 0;
    if (!wifi_connected && millis() - last_wifi_retry > 30000) {
        last_wifi_retry = millis();
        if (skip_network) {
            // Initial boot failed: try full re-init instead of simple reconnect
            WiFi.disconnect(true);
            delay(100);
            char ssid[64] = "", pass[64] = "";
            settings_load_wifi(ssid, pass, sizeof(ssid));
            WiFi.begin(ssid, pass);
        } else {
            WiFi.reconnect();
        }
        Serial.println("[WiFi] reconnecting...");
    }

    // WiFi came back up after boot failure — re-enable network
    if (skip_network && wifi_connected) {
        skip_network = false;
        Serial.println("[WiFi] connection restored — network re-enabled");
    }

    // WiFi dropped — restart BLE advertising so device can be re-provisioned
    if (wifi_was_connected && !wifi_connected) {
        Serial.println("[WiFi] disconnected — restarting BLE advertising");
        start_ble_advertising();
    }
    wifi_was_connected = wifi_connected;

    if (skip_network) return;
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        if (!mqtt.connected()) connect_mqtt();
        mqtt.loop();
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
        mqtt.publish(MQTT_LOG_TOPIC, encoded);
        batch_len = log_pop_batch(batch, sizeof(batch));
    }

    // Then drain LittleFS overflow file
    if (log_open_overflow_for_read()) {
        while (true) {
            size_t n = log_read_overflow_chunk(batch, sizeof(batch));
            if (n == 0) break;
            base64_encode(batch, n, encoded);
            mqtt.publish(MQTT_LOG_TOPIC, encoded);
        }
        log_close_overflow();
    }
}

static JsonDocument g_supa_doc;

static void send_one_log_entry_supabase(uint32_t timestamp_ms, const int16_t* v, const int16_t* i, const int16_t* p,
    const char* entry_type, const char* supabase_url, const char* anon_key,
    const char* device_key, const char* api_key) {
    if (!is_valid_uuid(api_key)) return;
    g_supa_doc.clear();
    g_supa_doc["p_device_key"] = device_key;
    g_supa_doc["p_device_api_key"] = api_key;

    JsonObject payload = g_supa_doc["p_payload"].to<JsonObject>();
    payload["source"] = "log";
    payload["entry_type"] = entry_type;

    // Same keys as live telemetry
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

    g_supa_doc["p_recorded_at"] = (uint32_t)log_to_epoch(timestamp_ms);

    static char buffer[512];
    size_t len = serializeJson(g_supa_doc, buffer);

    int rc = supabase_post("/rest/v1/rpc/insert_telemetry", buffer, len, supabase_url, anon_key);
    if (rc != 200 && rc != 201 && rc != 204) {
        print_http_error(g_supa_http, rc);
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
            send_one_log_entry_supabase(*abs_ts, abs_v, abs_i, abs_p, "base",
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
            send_one_log_entry_supabase(*abs_ts, abs_v, abs_i, abs_p, "delta",
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
    if (millis() - last_log_pub_ms < 1000) return; // rate limit: 1 log POST per second
    last_log_pub_ms = millis();

    if (ESP.getFreeHeap() < 12288) {
        Serial.println("[WARN] Low heap, skipping Supabase log publish");
        return;
    }
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;
    if (!is_valid_uuid(api_key)) return;

    // Static state machine: drains 1 entry per call to spread heap load
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
        uint8_t chunk[sizeof(BaseEntry)];
        size_t n = log_pop_batch(chunk, sizeof(chunk));
        if (n == 0) {
            state = ST_FS;
            return;
        }
        // Prepend carry
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
        // Only sent 1 entry; return to let heap recover
        return;
    }

    if (state == ST_FS) {
        if (!fs_file_open) {
            if (!log_open_overflow_for_read()) {
                state = ST_DONE;
                return;
            }
            fs_file_open = true;
        }
        uint8_t chunk[512];
        size_t n = log_read_overflow_chunk(chunk, sizeof(chunk));
        if (n == 0) {
            // EOF
            if (fs_carry_len > 0) {
                decode_and_send_log_entries(fs_carry, fs_carry_len, supabase_url, anon_key, device_key, api_key,
                    fs_v, fs_i, fs_p, &fs_ts, &fs_have);
                fs_carry_len = 0;
                return; // may have sent 1 entry; return to recover
            }
            log_close_overflow();
            fs_file_open = false;
            state = ST_DONE;
            return;
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
        return; // only sent 1 entry; return to let heap recover
    }

    if (state == ST_DONE) {
        // Reset for next cycle once buffer refills
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

void publish_data(const SensorData& data) {
    if (skip_network) return;

    g_pub_doc.clear();

    JsonArray ina3221Arr = g_pub_doc["ina3221"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
        JsonObject ch = ina3221Arr.add<JsonObject>();
        ch["v"] = data.ina3221_busV[i];
        ch["i"] = data.ina3221_current[i];
    }

#if ENABLE_INA226
    JsonObject ina226Obj = g_pub_doc["ina226"].to<JsonObject>();
    ina226Obj["v"] = data.ina226_busV;
    ina226Obj["i"] = data.ina226_current;
    ina226Obj["p"] = data.ina226_power;
#endif

    JsonArray adcArr = g_pub_doc["ads1115"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) {
        adcArr.add(data.ads1115_volts[i]);
    }

    g_pub_doc["log_entries"] = log_entries_count();
    g_pub_doc["log_buffer_kb"] = log_buffer_capacity() / 1024;
    g_pub_doc["log_overflow"] = log_has_overflow_file();
    g_pub_doc["log_overflow_bytes"] = log_overflow_file_size();

    // Virtual channels: compute V, I, P per channel from configured sources
    for (uint8_t ch = 0; ch < 4; ch++) {
        VirtualChannelConfig vc;
        if (!settings_load_virtual_channel(ch, &vc)) continue;
        char key[16];
        float v = 0, i = 0, p = 0;
        if (vc.voltage_src > 0) {
            v = get_sensor_voltage(vc.voltage_src, vc.voltage_idx, data);
        }
        if (vc.current_src > 0) {
            i = get_sensor_current(vc.current_src, vc.current_idx, data);
            if (vc.current_src == 3) {
                p = get_sensor_power(vc.current_src, vc.current_idx, data);
            } else if (vc.voltage_src > 0) {
                p = v * i;
            }
        }
        snprintf(key, sizeof(key), "ch%d_V", ch);
        g_pub_doc[key] = v;
        snprintf(key, sizeof(key), "ch%d_I", ch);
        g_pub_doc[key] = i;
        snprintf(key, sizeof(key), "ch%d_P", ch);
        g_pub_doc[key] = p;
    }

    char buffer[512];
    size_t len = serializeJson(g_pub_doc, buffer);

    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        mqtt.publish(MQTT_TOPIC, buffer, len);
    }

    publish_data_http(data, buffer, len);

    // Blynk virtual writes disabled — enable via platformio.ini lib_deps

    ble_notify_sensor_data(buffer, len);
}

void publish_data_supabase(const SensorData& data) {
    if (skip_network) return;
    static unsigned long last_pub_ms = 0;
    if (millis() - last_pub_ms < 1000) return; // rate limit: 1 POST per second
    last_pub_ms = millis();

    if (ESP.getFreeHeap() < 8192) {
        Serial.println("[WARN] Low heap, skipping Supabase publish");
        return;
    }
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;
    if (!is_valid_uuid(api_key)) {
        Serial.println("[WARN] device_api_key is not a valid UUID — skip Supabase publish. Use device_api_key from Supabase devices table (not sb_secret_...)");
        return;
    }

    uint32_t ms = millis();
    time_t epoch_s = (epoch_time > 0) ? epoch_time + ms / 1000 : time(nullptr);

    g_supa_doc.clear();
    g_supa_doc["p_device_key"] = device_key;
    g_supa_doc["p_device_api_key"] = api_key;

    JsonObject payload = g_supa_doc["p_payload"].to<JsonObject>();
    for (uint8_t i = 0; i < 3; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ina3221_v%d", i);
        payload[key] = data.ina3221_busV[i];
        snprintf(key, sizeof(key), "ina3221_i%d", i);
        payload[key] = data.ina3221_current[i];
    }
#if ENABLE_INA226
    payload["ina226_v"] = data.ina226_busV;
    payload["ina226_i"] = data.ina226_current;
    payload["ina226_p"] = data.ina226_power;
#endif
    for (uint8_t i = 0; i < 4; i++) {
        char key[16];
        snprintf(key, sizeof(key), "coulomb_mah%d", i);
        payload[key] = get_coulomb_mAh(i);
        snprintf(key, sizeof(key), "energy_wh%d", i);
        payload[key] = get_energy_Wh(i);
    }
    for (uint8_t i = 0; i < 4; i++) {
        BatteryConfig bat;
        if (settings_load_battery(i, &bat) && bat.capacity_mAh > 0) {
            float soc = bat.initial_soc_pct + (get_coulomb_mAh(i) / bat.capacity_mAh) * 100.0f;
            soc = soc < 0 ? 0 : soc > 100 ? 100 : soc;
            char key[16];
            snprintf(key, sizeof(key), "soc_pct%d", i);
            payload[key] = soc;
        }
    }
    payload["log_entries"] = log_entries_count();
    payload["log_buffer_kb"] = log_buffer_capacity() / 1024;
    payload["log_overflow"] = log_has_overflow_file();
    payload["log_overflow_bytes"] = log_overflow_file_size();

    // Virtual channels: compute V, I, P per channel from configured sources
    for (uint8_t ch = 0; ch < 4; ch++) {
        VirtualChannelConfig vc;
        if (!settings_load_virtual_channel(ch, &vc)) continue;
        char key[16];
        float v = 0, i = 0, p = 0;
        if (vc.voltage_src > 0) {
            v = get_sensor_voltage(vc.voltage_src, vc.voltage_idx, data);
        }
        if (vc.current_src > 0) {
            i = get_sensor_current(vc.current_src, vc.current_idx, data);
            if (vc.current_src == 3) {
                p = get_sensor_power(vc.current_src, vc.current_idx, data);
            } else if (vc.voltage_src > 0) {
                p = v * i;
            }
        }
        snprintf(key, sizeof(key), "ch%d_V", ch);
        payload[key] = v;
        snprintf(key, sizeof(key), "ch%d_I", ch);
        payload[key] = i;
        snprintf(key, sizeof(key), "ch%d_P", ch);
        payload[key] = p;
    }

    // Add stddev + spike flags for INA3221 channels
    for (uint8_t i = 0; i < 3; i++) {
        char key[16];
        SampleMeta m = sensor_get_meta(i); // 0-2 = INA3221 current
        snprintf(key, sizeof(key), "ina3221_i%d_stddev", i);
        payload[key] = m.stddev;
        snprintf(key, sizeof(key), "ina3221_i%d_spike", i);
        payload[key] = m.spike;
        m = sensor_get_meta(i + 3); // 3-5 = INA3221 voltage
        snprintf(key, sizeof(key), "ina3221_v%d_stddev", i);
        payload[key] = m.stddev;
        snprintf(key, sizeof(key), "ina3221_v%d_spike", i);
        payload[key] = m.spike;
    }

    JsonObject metadata = g_supa_doc["p_metadata"].to<JsonObject>();
    metadata["rssi"] = WiFi.RSSI();
    metadata["vcc"] = analogRead(0) / 4095.0f * 3.3f;
    metadata["uptime_s"] = millis() / 1000;

    g_supa_doc["p_recorded_at"] = (uint32_t)epoch_s;

    static char buffer[2048];
    size_t len = serializeJson(g_supa_doc, buffer);

    int rc = supabase_post("/rest/v1/rpc/insert_telemetry", buffer, len, supabase_url, anon_key);
    if (rc != 200 && rc != 201 && rc != 204) {
        print_http_error(g_supa_http, rc);
        if (rc == 400) {
            Serial.print("Payload preview: ");
            Serial.println(buffer);
        }
    }

    // Update calibration status table while baseline is collecting
    publish_calibration_status();
}

static JsonDocument g_cal_doc;

void sync_calibration_to_supabase() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < 4096) return;
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    ChannelCalibration cal;
    if (!settings_load_channel_calibration(&cal)) return;

    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/device_channels?device_key=eq.%s", device_key);

    g_cal_doc.clear();
    JsonObject cal_obj = g_cal_doc["channel_calibration"].to<JsonObject>();
    JsonArray volt_offset = cal_obj["volt_offset_mv"].to<JsonArray>();
    JsonArray volt_gain = cal_obj["volt_gain"].to<JsonArray>();
    JsonArray curr_offset = cal_obj["curr_offset_ma"].to<JsonArray>();
    JsonArray curr_gain = cal_obj["curr_gain"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
        volt_offset.add(cal.volt_offset_mv[i]);
        volt_gain.add(cal.volt_gain[i]);
        curr_offset.add(cal.curr_offset_ma[i]);
        curr_gain.add(cal.curr_gain[i]);
    }

    static char buffer[512];
    size_t len = serializeJson(g_cal_doc, buffer);
    supabase_http_reset();
    g_supa_http.addHeader("Prefer", "precision=exact");
    int rc = supabase_patch(path, buffer, len, supabase_url, anon_key);
    supabase_http_reset();
    if (rc >= 200 && rc < 300) {
        Serial.println("Calibration synced to Supabase");
    } else {
        print_http_error(g_supa_http, rc);
    }
}

void sync_ble_pin_to_supabase() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < 4096) return;
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    uint32_t pin = settings_load_ble_pin();
    char pin_str[16];
    snprintf(pin_str, sizeof(pin_str), "%lu", (unsigned long)pin);

    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/device_channels?device_key=eq.%s", device_key);

    g_cal_doc.clear();
    g_cal_doc["ble_pin"] = pin_str;
    static char buffer[256];
    size_t len = serializeJson(g_cal_doc, buffer);
    int rc = supabase_patch(path, buffer, len, supabase_url, anon_key);
    if (rc >= 200 && rc < 300) {
        Serial.println("BLE PIN synced to Supabase");
    } else {
        print_http_error(g_supa_http, rc);
    }
}

void apply_settings_posthook(const char* cmd_type) {
    if (strcmp(cmd_type, "set_wifi") == 0) {
        char ssid[64] = "", pass[64] = "";
        if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
            WiFi.disconnect(true);
            delay(100);
            WiFi.begin(ssid, pass);
            Serial.println("[CMD] WiFi reconnecting with new credentials");
        }
    } else if (strcmp(cmd_type, "set_mqtt") == 0) {
        char broker[64], topic[64];
        uint16_t port;
        if (settings_load_mqtt(broker, &port, topic, sizeof(broker))) {
            mqtt.disconnect();
            mqtt.setServer(broker, port);
            Serial.println("[CMD] MQTT reconnecting with new broker");
        }
    } else if (strcmp(cmd_type, "set_supabase") == 0) {
        supabase_http_reset();
        Serial.println("[CMD] Supabase client reset with new URL/key");
    }
}

void publish_calibration_status() {
    if (skip_network) return;
    if (!sensor_is_calibrating()) return;  // nothing to report
    static unsigned long last_cal_pub_ms = 0;
    if (millis() - last_cal_pub_ms < 5000) return; // rate limit: 1 cal publish per 5s
    last_cal_pub_ms = millis();
    if (ESP.getFreeHeap() < 4096) return;

    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    float stddev_out[8];
    uint8_t tick_count;
    sensor_get_baseline_progress(stddev_out, &tick_count);

    g_cal_doc.clear();
    g_cal_doc["device_key"] = device_key;
    g_cal_doc["calibrating"] = sensor_is_calibrating();
    g_cal_doc["baseline_tick"] = tick_count;
    JsonObject sd = g_cal_doc["baseline_stddev"].to<JsonObject>();
    char key[16];
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof(key), "ina3221_i%d", i);
        sd[key] = stddev_out[i];
    }
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof(key), "ina3221_v%d", i);
        sd[key] = stddev_out[i + 3];
    }
    g_cal_doc["updated_at"] = "now";

    static char buffer[512];
    size_t len = serializeJson(g_cal_doc, buffer);
    // Reset persistent client so the Prefer header is added to a fresh connection only
    supabase_http_reset();
    g_supa_http.addHeader("Prefer", "resolution=merge-duplicates");
    int rc = supabase_patch("/rest/v1/sensor_calibration_status", buffer, len, supabase_url, anon_key);
    // Force reset after PATCH so the custom header doesn't leak into subsequent POSTs
    supabase_http_reset();
    if (rc >= 200 && rc < 300) {
        Serial.printf("[CALIB] status published: tick=%d\n", tick_count);
    } else {
        print_http_error(g_supa_http, rc);
    }
}

bool get_ble_pin_from_supabase(char* pin_str, size_t len) {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return false;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return false;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return false;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return false;

    char path[256];
    snprintf(path, sizeof(path), "/rest/v1/device_channels?device_key=eq.%s&select=ble_pin", device_key);
    int rc = supabase_get(path, supabase_url, anon_key);
    bool ok = false;
    if (rc == 200) {
        // Parse: [{"ble_pin":"123456"}] — read into stack buffer, no String heap allocation
        char resp[64];
        size_t resp_len = 0;
        WiFiClient* stream = g_supa_http.getStreamPtr();
        while (stream->available() && resp_len < sizeof(resp) - 1) {
            resp[resp_len++] = stream->read();
        }
        resp[resp_len] = '\0';
        const char* needle = "\"ble_pin\":\"";
        const char* p = strstr(resp, needle);
        if (p) {
            p += strlen(needle);
            char* q = strchr(p, '"');
            if (q && (size_t)(q - p) < (int)len) {
                memcpy(pin_str, p, q - p);
                pin_str[q - p] = '\0';
                ok = pin_str[0] != '\0';
            }
        }
    }
    return ok;
}

void check_settings_commands() {
    if (skip_network) return;
    if (ESP.getFreeHeap() < 4096) return;
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    static unsigned long last_check = 0;
    if (millis() - last_check < 30000) return;  // poll every 30s
    last_check = millis();

    g_cal_doc.clear();
    g_cal_doc["p_device_key"] = device_key;
    char buffer[256];
    size_t len = serializeJson(g_cal_doc, buffer);

    int rc = supabase_post("/rest/v1/rpc/claim_settings_command", buffer, len, supabase_url, anon_key);

    if (rc == 200) {
        // Read into stack buffer — no String heap allocation
        char resp_buf[512];
        size_t resp_len = 0;
        WiFiClient* stream = g_supa_http.getStreamPtr();
        while (stream->available() && resp_len < sizeof(resp_buf) - 1) {
            resp_buf[resp_len++] = stream->read();
        }
        resp_buf[resp_len] = '\0';

        if (resp_len == 0 || strncmp(resp_buf, "null", 4) == 0) return;

        // Expected: {"cmd_type":"set_wifi","payload":{...}}  or  null
        g_cal_doc.clear();
        DeserializationError err = deserializeJson(g_cal_doc, resp_buf);
        if (err) {
            Serial.printf("[SETTINGS] parse error: %s | body: %.200s\n", err.c_str(), resp_buf);
            return;
        }

        const char* cmd_type = g_cal_doc["cmd_type"] | "";
        if (strlen(cmd_type) == 0) return;

        // payload may be a JSON object (jsonb) or a string; handle both
        char payload_buf[512];
        JsonVariant payload_var = g_cal_doc["payload"];
        if (payload_var.is<const char*>()) {
            strlcpy(payload_buf, payload_var.as<const char*>(), sizeof(payload_buf));
        } else {
            serializeJson(payload_var, payload_buf, sizeof(payload_buf));
        }
        apply_settings_command(cmd_type, payload_buf);
        apply_settings_posthook(cmd_type);
    } else {
        print_http_error(g_supa_http, rc);
    }
}
