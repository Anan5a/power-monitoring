#include "connectivity_manager.h"
#include "config.h"
#include "settings_manager.h"
#include "data_logger.h"
#include "ble_provisioner.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static char ip_str[16] = "0.0.0.0";

static void connect_wifi() {
    char ssid[64], pass[64];
    if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
        WiFi.begin(ssid, pass);
    } else {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    strlcpy(ip_str, WiFi.localIP().toString().c_str(), sizeof(ip_str));
    Serial.println("\nWiFi connected");
}

static void connect_mqtt() {
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

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    connect_mqtt();

    Blynk.config(BLYNK_AUTH_TOKEN);
    if (Blynk.connect()) {
        Serial.println("Blynk connected");
    } else {
        Serial.println("Blynk connect failed");
    }
}

void loop_connectivity() {
    if (!mqtt.connected()) {
        connect_mqtt();
    }
    mqtt.loop();
    Blynk.run();
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
    if (!mqtt.connected()) return;
    uint8_t batch[512];
    size_t batch_len = log_pop_batch(batch, sizeof(batch));
    if (batch_len == 0) return;
    char encoded[700];
    base64_encode(batch, batch_len, encoded);
    mqtt.publish(MQTT_LOG_TOPIC, encoded);
}

void publish_data(const SensorData& data) {
    JsonDocument doc;

    JsonArray ina3221Arr = doc["ina3221"].to<JsonArray>();
    for (uint8_t i = 0; i < 3; i++) {
        JsonObject ch = ina3221Arr.add<JsonObject>();
        ch["v"] = data.ina3221_busV[i];
        ch["i"] = data.ina3221_current[i];
    }

    JsonObject ina226Obj = doc["ina226"].to<JsonObject>();
    ina226Obj["v"] = data.ina226_busV;
    ina226Obj["i"] = data.ina226_current;
    ina226Obj["p"] = data.ina226_power;

    JsonArray adcArr = doc["ads1115"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) {
        adcArr.add(data.ads1115_volts[i]);
    }

    doc["log_entries"] = log_entries_count();
    doc["log_overflow"] = log_has_overflow_file();
    doc["log_overflow_bytes"] = log_overflow_file_size();

    char buffer[512];
    size_t len = serializeJson(doc, buffer);
    mqtt.publish(MQTT_TOPIC, buffer, len);

    publish_data_http(data, buffer, len);

    Blynk.virtualWrite(V0, data.ina3221_busV[0]);
    Blynk.virtualWrite(V1, data.ina3221_current[0]);
    Blynk.virtualWrite(V2, data.ina226_busV);
    Blynk.virtualWrite(V3, data.ina226_current);
    Blynk.virtualWrite(V4, data.ina226_power);
    Blynk.virtualWrite(V5, data.ads1115_volts[0]);

    ble_notify_sensor_data(buffer, len);
}
