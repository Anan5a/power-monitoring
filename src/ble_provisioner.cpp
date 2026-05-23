#include "ble_provisioner.h"
#include "config.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

static BLECharacteristic* pCmdChar = nullptr;
static BLECharacteristic* pRespChar = nullptr;
static BLECharacteristic* pStatusChar = nullptr;
static BLECharacteristic* pSensorChar = nullptr;
static bool bleClientConnected = false;

static void handle_command(const char* json);

class ProvServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { bleClientConnected = true; }
    void onDisconnect(BLEServer* pServer) { bleClientConnected = false; }
};

class CmdCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        std::string val = pCharacteristic->getValue();
        if (val.empty()) return;
        handle_command(val.c_str());
    }
};

static void send_response(const char* msg) {
    if (pRespChar) {
        pRespChar->setValue(std::string(msg));
        pRespChar->notify();
    }
}

static bool check_pin(JsonDocument& doc) {
    uint32_t expected = settings_load_ble_pin();
    if (expected == 0) return true; // no security
    uint32_t provided = doc["pin"] | 0;
    if (provided != expected) {
        send_response("{\"ok\":false,\"error\":\"invalid_pin\"}");
        return false;
    }
    return true;
}

static void handle_command(const char* json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { send_response("{\"ok\":false,\"error\":\"bad_json\"}"); return; }

    const char* cmd = doc["cmd"] | "";
    if (strcmp(cmd, "set_wifi") == 0) {
        if (!check_pin(doc)) return;
        settings_save_wifi(doc["ssid"], doc["pass"]);
        send_response("{\"ok\":true,\"msg\":\"wifi_saved_reboot\"}");
    } else if (strcmp(cmd, "set_mqtt") == 0) {
        if (!check_pin(doc)) return;
        settings_save_mqtt(doc["broker"], doc["port"], doc["topic"]);
        send_response("{\"ok\":true,\"msg\":\"mqtt_saved\"}");
    } else if (strcmp(cmd, "set_http") == 0) {
        if (!check_pin(doc)) return;
        settings_save_http_endpoint(doc["url"], doc["token"]);
        settings_save_http_enabled(doc["enabled"] | true);
        send_response("{\"ok\":true,\"msg\":\"http_saved\"}");
    } else if (strcmp(cmd, "set_relay") == 0) {
        if (!check_pin(doc)) return;
        RelayRule rt = {};
        rt.channel = doc["channel"] | 0;
        rt.overcurrent_A = doc["overcurrent_A"] | 0.0f;
        rt.undervoltage_V = doc["undervoltage_V"] | 0.0f;
        rt.soc_low_pct = doc["soc_low_pct"] | 0.0f;
        rt.soc_high_pct = doc["soc_high_pct"] | 0.0f;
        rt.trip_delay_ms = doc["trip_delay_ms"] | 1000;
        rt.reset_delay_ms = doc["reset_delay_ms"] | 5000;
        rt.gpio_pin = doc["gpio_pin"] | 25;
        rt.active_high = doc["active_high"] | false;
        rt.enabled = doc["enabled"] | true;
        settings_save_relay(doc["idx"] | 0, &rt);
        send_response("{\"ok\":true,\"msg\":\"relay_saved\"}");
    } else if (strcmp(cmd, "set_battery") == 0) {
        if (!check_pin(doc)) return;
        BatteryConfig bat = {};
        bat.channel = doc["channel"] | 0;
        bat.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bat.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        settings_save_battery(bat.channel, &bat);
        send_response("{\"ok\":true,\"msg\":\"battery_saved\"}");
    } else if (strcmp(cmd, "set_pin") == 0) {
        uint32_t old = settings_load_ble_pin();
        uint32_t provided = doc["old_pin"] | 0;
        if (old != 0 && provided != old) {
            send_response("{\"ok\":false,\"error\":\"invalid_old_pin\"}");
            return;
        }
        settings_save_ble_pin(doc["new_pin"] | 0);
        send_response("{\"ok\":true,\"msg\":\"pin_updated\"}");
    } else if (strcmp(cmd, "get_status") == 0) {
        JsonDocument resp;
        resp["ok"] = true;
        resp["entries"] = log_entries_count();
        resp["overflow"] = log_has_overflow_file();
        resp["relay_count"] = settings_load_relay_count();
        char buf[256];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "reset_coulomb") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        reset_coulomb_counter(ch);
        send_response("{\"ok\":true,\"msg\":\"coulomb_reset\"}");
    } else if (strcmp(cmd, "get_relay") == 0) {
        if (!check_pin(doc)) return;
        uint8_t idx = doc["idx"] | 0;
        RelayRule rt;
        JsonDocument resp;
        if (settings_load_relay(idx, &rt)) {
            resp["ok"] = true;
            resp["idx"] = idx;
            resp["channel"] = rt.channel;
            resp["overcurrent_A"] = rt.overcurrent_A;
            resp["undervoltage_V"] = rt.undervoltage_V;
            resp["soc_low_pct"] = rt.soc_low_pct;
            resp["soc_high_pct"] = rt.soc_high_pct;
            resp["trip_delay_ms"] = rt.trip_delay_ms;
            resp["reset_delay_ms"] = rt.reset_delay_ms;
            resp["gpio_pin"] = rt.gpio_pin;
            resp["active_high"] = rt.active_high;
            resp["enabled"] = rt.enabled;
        } else {
            resp["ok"] = false;
            resp["error"] = "relay_not_found";
        }
        char buf[384];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "get_battery") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        BatteryConfig bat;
        JsonDocument resp;
        if (settings_load_battery(ch, &bat)) {
            resp["ok"] = true;
            resp["channel"] = bat.channel;
            resp["capacity_mAh"] = bat.capacity_mAh;
            resp["initial_soc_pct"] = bat.initial_soc_pct;
        } else {
            resp["ok"] = false;
            resp["error"] = "battery_not_found";
        }
        char buf[256];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "get_wifi") == 0) {
        if (!check_pin(doc)) return;
        char ssid[64] = "", pass[64] = "";
        JsonDocument resp;
        if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
            resp["ok"] = true;
            resp["ssid"] = ssid;
            resp["pass"] = "***";
        } else {
            resp["ok"] = false;
            resp["error"] = "wifi_not_set";
        }
        char buf[128];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "get_mqtt") == 0) {
        if (!check_pin(doc)) return;
        char broker[64] = "", topic[64] = "";
        uint16_t port = 0;
        JsonDocument resp;
        if (settings_load_mqtt(broker, &port, topic, sizeof(broker))) {
            resp["ok"] = true;
            resp["broker"] = broker;
            resp["port"] = port;
            resp["topic"] = topic;
        } else {
            resp["ok"] = false;
            resp["error"] = "mqtt_not_set";
        }
        char buf[128];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "get_http") == 0) {
        if (!check_pin(doc)) return;
        char url[128] = "", token[64] = "";
        JsonDocument resp;
        if (settings_load_http_endpoint(url, token, sizeof(url))) {
            resp["ok"] = true;
            resp["url"] = url;
            resp["token"] = "***";
            resp["enabled"] = settings_load_http_enabled();
        } else {
            resp["ok"] = false;
            resp["error"] = "http_not_set";
        }
        char buf[128];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "factory_reset") == 0) {
        if (!check_pin(doc)) return;
        settings_factory_reset();
        send_response("{\"ok\":true,\"msg\":\"factory_reset_done_reboot\"}");
    } else {
        send_response("{\"ok\":false,\"error\":\"unknown_cmd\"}");
    }
}

void init_ble_provisioner() {
    BLEDevice::init(BT_DEVICE_NAME);
    BLEServer* pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ProvServerCallbacks());
    BLEService* pService = pServer->createService(BLE_SERVICE_UUID);

    pCmdChar = pService->createCharacteristic(
        BLE_CHAR_CMD_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCmdChar->setCallbacks(new CmdCallbacks());

    pRespChar = pService->createCharacteristic(
        BLE_CHAR_RESP_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pRespChar->addDescriptor(new BLE2902());

    pStatusChar = pService->createCharacteristic(
        BLE_CHAR_STATUS_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pStatusChar->addDescriptor(new BLE2902());

    pSensorChar = pService->createCharacteristic(
        BLE_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pSensorChar->addDescriptor(new BLE2902());

    pService->start();
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    BLEDevice::startAdvertising();
}

void loop_ble_provisioner() {
    if (!bleClientConnected || !pStatusChar) return;
    // Broadcast status every 10s
    static unsigned long last_status = 0;
    if (millis() - last_status >= 10000) {
        last_status = millis();
        JsonDocument doc;
        doc["uptime_s"] = millis() / 1000;
        doc["entries"] = log_entries_count();
        doc["overflow"] = log_has_overflow_file();
        char buf[128];
        serializeJson(doc, buf);
        pStatusChar->setValue(std::string(buf));
        pStatusChar->notify();
    }
}

void ble_notify_sensor_data(const char* data, size_t len) {
    if (bleClientConnected && pSensorChar) {
        pSensorChar->setValue(std::string(data, len));
        pSensorChar->notify();
    }
}
