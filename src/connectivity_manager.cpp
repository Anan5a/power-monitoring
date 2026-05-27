#include "connectivity_manager.h"
#include "config.h"
#include "settings_manager.h"
#include "data_logger.h"
#include "ble_provisioner.h"
#include "coulomb_counter.h"
#include "sensor_manager.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <time.h>

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static char ip_str[16] = "0.0.0.0";

static bool skip_network = false;

static time_t epoch_time = 0;

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
    strlcpy(ip_str, WiFi.localIP().toString().c_str(), sizeof(ip_str));
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

void publish_data_http(const SensorData& data, const char* json_buffer, size_t json_len) {
    if (!settings_load_http_enabled()) return;
    char url[128], token[64];
    if (!settings_load_http_endpoint(url, token, sizeof(url))) return;
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    if (strlen(token) > 0) http.addHeader("Authorization", token);
    int rc = http.POST((uint8_t*)json_buffer, json_len);
    http.end();
    if (rc != 200 && rc != 202) {
        Serial.print("HTTP publish failed: "); Serial.println(rc);
    }
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

    if (strcmp(BLYNK_AUTH_TOKEN, "YOUR_BLYNK_TOKEN") != 0) {
        Blynk.config(BLYNK_AUTH_TOKEN);
        if (Blynk.connect()) {
            Serial.println("Blynk connected");
        } else {
            Serial.println("Blynk connect failed");
        }
    } else {
        Serial.println("Blynk: not configured (skip)");
    }
}

void loop_connectivity() {
    if (skip_network) return;
    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        if (!mqtt.connected()) connect_mqtt();
        mqtt.loop();
    }
    if (strcmp(BLYNK_AUTH_TOKEN, "YOUR_BLYNK_TOKEN") != 0) {
        Blynk.run();
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
    size_t batch_len = log_pop_batch(batch, sizeof(batch));
    if (batch_len == 0) return;
    char encoded[700];
    base64_encode(batch, batch_len, encoded);
    mqtt.publish(MQTT_LOG_TOPIC, encoded);
}

void publish_data(const SensorData& data) {
    if (skip_network) return;

    JsonDocument doc;

    JsonArray ina3221Arr = doc["ina3221"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
        JsonObject ch = ina3221Arr.add<JsonObject>();
        ch["v"] = data.ina3221_busV[i];
        ch["i"] = data.ina3221_current[i];
    }

#if ENABLE_INA226
    JsonObject ina226Obj = doc["ina226"].to<JsonObject>();
    ina226Obj["v"] = data.ina226_busV;
    ina226Obj["i"] = data.ina226_current;
    ina226Obj["p"] = data.ina226_power;
#endif

    JsonArray adcArr = doc["ads1115"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) {
        adcArr.add(data.ads1115_volts[i]);
    }

    doc["log_entries"] = log_entries_count();
    doc["log_overflow"] = log_has_overflow_file();
    doc["log_overflow_bytes"] = log_overflow_file_size();

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
        if (vc.voltage_src > 0 && vc.current_src == 0) {
            // Current-only source (INA226 with separate power calc)
        }
        snprintf(key, sizeof(key), "ch%d_V", ch);
        doc[key] = v;
        snprintf(key, sizeof(key), "ch%d_I", ch);
        doc[key] = i;
        snprintf(key, sizeof(key), "ch%d_P", ch);
        doc[key] = p;
    }

    char buffer[512];
    size_t len = serializeJson(doc, buffer);

    char mqtt_broker[64]; uint16_t mqtt_port; char mqtt_topic[64];
    if (settings_load_mqtt(mqtt_broker, &mqtt_port, mqtt_topic, sizeof(mqtt_broker))) {
        mqtt.publish(MQTT_TOPIC, buffer, len);
    }

    publish_data_http(data, buffer, len);

#if ENABLE_INA226
    Blynk.virtualWrite(V0, data.ina3221_busV[0]);
    Blynk.virtualWrite(V1, data.ina3221_current[0]);
    Blynk.virtualWrite(V2, data.ina226_busV);
    Blynk.virtualWrite(V3, data.ina226_current);
    Blynk.virtualWrite(V4, data.ina226_power);
    Blynk.virtualWrite(V5, data.ads1115_volts[0]);
#endif

    ble_notify_sensor_data(buffer, len);
}

void publish_data_supabase(const SensorData& data) {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    HTTPClient http;
    http.begin(String(supabase_url) + "/rest/v1/rpc/insert_telemetry");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", anon_key);
    http.addHeader("Authorization", "Bearer " + String(anon_key));

    uint32_t ms = millis();
    time_t epoch_s = (epoch_time > 0) ? epoch_time + ms / 1000 : time(nullptr);

    JsonDocument doc;
    doc["p_device_key"] = device_key;
    doc["p_device_api_key"] = api_key;

    JsonObject payload = doc["p_payload"].to<JsonObject>();
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

    JsonObject metadata = doc["p_metadata"].to<JsonObject>();
    metadata["rssi"] = WiFi.RSSI();
    metadata["vcc"] = analogRead(0) / 4095.0f * 3.3f;
    metadata["uptime_s"] = millis() / 1000;

    doc["p_recorded_at"] = (uint32_t)epoch_s;

    char buffer[1024];
    size_t len = serializeJson(doc, buffer);

    int rc = http.POST((uint8_t*)buffer, len);
    http.end();
    if (rc != 200 && rc != 201 && rc != 204) {
        Serial.print("Supabase publish failed: "); Serial.println(rc);
    }
}

void sync_calibration_to_supabase() {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    ChannelCalibration cal;
    if (!settings_load_channel_calibration(&cal)) return;

    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/device_channels?device_key=eq.%s", supabase_url, device_key);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", anon_key);
    http.addHeader("Authorization", "Bearer " + String(anon_key));
    http.addHeader("Prefer", "precision=exact");

    JsonDocument doc;
    JsonObject cal_obj = doc["channel_calibration"].to<JsonObject>();
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

    char buffer[512];
    size_t len = serializeJson(doc, buffer);
    int rc = http.sendRequest("PATCH", (uint8_t*)buffer, len);
    http.end();
    if (rc >= 200 && rc < 300) {
        Serial.println("Calibration synced to Supabase");
    } else {
        Serial.print("Calibration sync failed: "); Serial.println(rc);
    }
}

void sync_ble_pin_to_supabase() {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    uint32_t pin = settings_load_ble_pin();
    char pin_str[16];
    snprintf(pin_str, sizeof(pin_str), "%lu", (unsigned long)pin);

    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/device_channels?device_key=eq.%s", supabase_url, device_key);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", anon_key);
    http.addHeader("Authorization", "Bearer " + String(anon_key));

    JsonDocument doc;
    doc["ble_pin"] = pin_str;
    char buffer[256];
    size_t len = serializeJson(doc, buffer);
    int rc = http.sendRequest("PATCH", (uint8_t*)buffer, len);
    http.end();
    if (rc >= 200 && rc < 300) {
        Serial.println("BLE PIN synced to Supabase");
    } else {
        Serial.print("BLE PIN sync failed: "); Serial.println(rc);
    }
}

bool get_ble_pin_from_supabase(char* pin_str, size_t len) {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return false;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return false;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return false;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return false;

    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url), "%s/rest/v1/device_channels?device_key=eq.%s&select=ble_pin", supabase_url, device_key);
    http.begin(url);
    http.addHeader("apikey", anon_key);
    http.addHeader("Authorization", "Bearer " + String(anon_key));

    int rc = http.GET();
    bool ok = false;
    if (rc == 200) {
        String body = http.getString();
        // Response: [{"ble_pin":"123456"}]
        int q1 = body.indexOf('"');
        if (q1 >= 0) {
            int q2 = body.indexOf('"', q1 + 1);
            if (q2 > q1) {
                strlcpy(pin_str, body.substring(q1 + 1, q2).c_str(), len);
                ok = strlen(pin_str) > 0;
            }
        }
    }
    http.end();
    return ok;
}

void check_settings_commands() {
    char supabase_url[128], anon_key[128], device_key[64], api_key[64];
    if (!settings_load_supabase_url(supabase_url, sizeof(supabase_url))) return;
    if (!settings_load_supabase_anon_key(anon_key, sizeof(anon_key))) return;
    if (!settings_load_supabase_device_key(device_key, sizeof(device_key))) return;
    if (!settings_load_supabase_api_key(api_key, sizeof(api_key))) return;

    static unsigned long last_check = 0;
    if (millis() - last_check < 30000) return;  // poll every 30s
    last_check = millis();

    HTTPClient http;
    http.begin(String(supabase_url) + "/rest/v1/rpc/claim_settings_command");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", anon_key);
    http.addHeader("Authorization", "Bearer " + String(anon_key));

    JsonDocument doc;
    doc["p_device_key"] = device_key;
    char buffer[256];
    size_t len = serializeJson(doc, buffer);

    int rc = http.POST((uint8_t*)buffer, len);
    http.end();

    if (rc == 200) {
        String body = http.getString();
        if (body.length() < 5 || body == "null") return;  // no pending command

        // Expected: {"cmd_type":"set_wifi","payload":{...}}
        JsonDocument resp;
        DeserializationError err = deserializeJson(resp, body.c_str());
        if (err) { Serial.println("[SETTINGS] parse error"); return; }

        const char* cmd_type = resp["cmd_type"] | "";
        const char* payload = resp["payload"] | "{}";
        if (strlen(cmd_type) > 0) {
            apply_settings_command(cmd_type, payload);
        }
    }
}
