#include "ble_provisioner.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "relay_controller.h"
#include "connectivity_manager.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

static NimBLECharacteristic* pCmdChar = nullptr;
static NimBLECharacteristic* pRespChar = nullptr;
static NimBLECharacteristic* pStatusChar = nullptr;
static NimBLECharacteristic* pSensorChar = nullptr;
static bool bleClientConnected = false;
static bool ble_initialized = false;

// Rate limiting: track commands per connection window
#define RATE_WINDOW_MS    10000   // 10-second window
#define MAX_COMMANDS      10     // max 10 commands per window per connection
static uint16_t rate_cmd_count = 0;
static unsigned long rate_window_start = 0;
static unsigned long rate_last_cmd = 0;

static void handle_command(const char* json);

static bool ble_advertising_active = false;

class ProvServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        bleClientConnected = true;
        rate_window_start = millis();
        rate_cmd_count = 0;
        Serial.printf("[BLE] client connected (addr=%s)\n", connInfo.getAddress().toString().c_str());
        // Do NOT update connection params — let NimBLE use the defaults negotiated by the central.
        // Windows/Web Bluetooth often disconnects immediately when the peripheral overrides params.
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        bleClientConnected = false;
        // advertiseOnDisconnect(true) handles restart automatically
        Serial.printf("[BLE] client disconnected (reason=%d)\n", reason);
    }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        std::string val = pCharacteristic->getValue();
        Serial.printf("[BLE] onWrite len=%d\n", val.length());
        if (val.empty()) return;
        handle_command(val.c_str());
    }
};

static void send_response(const char* msg) {
    if (!bleClientConnected || !pRespChar) { Serial.println(F("[BLE] send_response: not connected or pRespChar is null")); return; }
    size_t len = strlen(msg);
    pRespChar->notify((const uint8_t*)msg, len);
    Serial.printf("[BLE] sent resp: %s\n", msg);
}

static bool check_pin(JsonDocument& doc) {
    uint32_t expected = settings_load_ble_pin();
    if (expected == 0) return true; // no security
    // Accept PIN as number or as string (dashboard sends "123456" as JSON string)
    uint32_t provided = 0;
    if (doc["pin"].is<const char*>()) {
        provided = atoi(doc["pin"].as<const char*>());
    } else {
        provided = doc["pin"] | 0;
    }
    if (provided != expected) {
        send_response("{\"ok\":false,\"error\":\"invalid_pin\"}");
        return false;
    }
    return true;
}

static bool check_rate_limit() {
    unsigned long now = millis();
    if (now - rate_window_start >= RATE_WINDOW_MS) {
        rate_window_start = now;
        rate_cmd_count = 0;
    }
    if (now - rate_last_cmd < 100) {
        rate_cmd_count++;
        if (rate_cmd_count > MAX_COMMANDS) {
            Serial.println(F("[BLE] rate limited"));
            send_response("{\"ok\":false,\"error\":\"rate_limited\"}");
            return false;
        }
    } else {
        rate_cmd_count = 0; // reset on slow command (normal usage)
    }
    rate_last_cmd = now;
    return true;
}

static void handle_command(const char* json) {
    Serial.printf("[BLE] command: %s\n", json);
    if (!check_rate_limit()) { Serial.println(F("[BLE] rate limited")); return; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.println(F("[BLE] bad json")); send_response("{\"ok\":false,\"error\":\"bad_json\"}"); return; }

    const char* cmd = doc["cmd"] | "";
    Serial.printf("[BLE] cmd: %s\n", cmd);
    if (strcmp(cmd, "set_wifi") == 0) {
        uint32_t pin = doc["pin"] | 0;
        Serial.printf("[BLE] set_wifi pin=%lu\n", pin);
        uint32_t stored_pin = settings_load_ble_pin();
        Serial.printf("[BLE] stored_pin=%lu expected=%lu\n", stored_pin, pin);
        if (!check_pin(doc)) { Serial.println("[BLE] pin check failed"); return; }
        settings_save_wifi(doc["ssid"], doc["pass"]);
        apply_settings_posthook("set_wifi");
        send_response("{\"ok\":true,\"msg\":\"wifi_saved_reconnecting\"}");
        Serial.println("[BLE] wifi saved");
    } else if (strcmp(cmd, "set_mqtt") == 0) {
        if (!check_pin(doc)) return;
        settings_save_mqtt(doc["broker"], doc["port"], doc["topic"]);
        apply_settings_posthook("set_mqtt");
        send_response("{\"ok\":true,\"msg\":\"mqtt_saved\"}");
    } else if (strcmp(cmd, "set_http") == 0) {
        if (!check_pin(doc)) return;
        settings_save_http_endpoint(doc["url"], doc["token"]);
        settings_save_http_enabled(doc["enabled"] | true);
        send_response("{\"ok\":true,\"msg\":\"http_saved\"}");
    } else if (strcmp(cmd, "set_relay") == 0) {
        if (!check_pin(doc)) return;
        RelayRule rt = {};
        rt.channel = doc["idx"] | 0;
        rt.overcurrent_A = doc["overcurrent_A"] | 0.0f;
        rt.undervoltage_V = doc["undervoltage_V"] | 0.0f;
        rt.soc_low_pct = doc["soc_low_pct"] | 0.0f;
        rt.soc_high_pct = doc["soc_high_pct"] | 0.0f;
        rt.trip_delay_ms = doc["trip_delay_ms"] | 1000;
        rt.reset_delay_ms = doc["reset_delay_ms"] | 5000;
        uint8_t idx = doc["idx"] | 0;
        uint8_t default_relay_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
        rt.gpio_pin = default_relay_pins[idx];  // use board default, not hardcoded 25
        rt.active_high = doc["active_high"] | false;
        rt.enabled = doc["enabled"] | true;
        rt.is_energized = get_relay_state(idx);  // preserve current state
        settings_save_relay(idx, &rt);
        publish_relay_state(idx, rt.is_energized);  // sync to Supabase
        send_response("{\"ok\":true,\"msg\":\"relay_saved\"}");
    } else if (strcmp(cmd, "set_battery") == 0) {
        if (!check_pin(doc)) return;
        BatteryConfig bat = {};
        bat.channel = doc["channel"] | 0;
        bat.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bat.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        settings_save_battery(bat.channel, &bat);
        send_response("{\"ok\":true,\"msg\":\"battery_saved\"}");
    } else if (strcmp(cmd, "set_shunt") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        settings_save_shunt(ch, doc["ohms"] | 0.0f);
        apply_settings_posthook("set_shunt");
        send_response("{\"ok\":true,\"msg\":\"shunt_saved\"}");
    } else if (strcmp(cmd, "set_volt_ratio") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        settings_save_volt_ratio(ch, doc["ratio"] | 0.0f);
        apply_settings_posthook("set_volt_ratio");
        send_response("{\"ok\":true,\"msg\":\"vratio_saved\"}");
    } else if (strcmp(cmd, "set_resistors") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        settings_save_resistors(ch, doc["r_high"] | 0.0f, doc["r_low"] | 0.0f);
        apply_settings_posthook("set_resistors");
        send_response("{\"ok\":true,\"msg\":\"resistors_saved\"}");
    } else if (strcmp(cmd, "set_pin") == 0) {
        uint32_t old = settings_load_ble_pin();
        uint32_t provided = doc["old_pin"] | 0;
        if (old != 0 && provided != old) {
            send_response("{\"ok\":false,\"error\":\"invalid_old_pin\"}");
            return;
        }
        settings_save_ble_pin(doc["new_pin"] | 0);
        sync_ble_pin_to_supabase();
        send_response("{\"ok\":true,\"msg\":\"pin_updated\"}");
    } else if (strcmp(cmd, "get_status") == 0) {
        JsonDocument resp;
        resp["ok"] = true;
        resp["entries"] = log_entries_count();
        resp["buffer_kb"] = log_buffer_capacity() / 1024;
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
    } else if (strcmp(cmd, "calibrate_baseline") == 0) {
        if (!check_pin(doc)) return;
        sensor_calibrate_baseline();
        send_response("{\"ok\":true,\"msg\":\"baseline_calibration_started\"}");
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
    } else if (strcmp(cmd, "set_supabase") == 0) {
        if (!check_pin(doc)) return;
        settings_save_supabase_url(doc["url"] | "");
        settings_save_supabase_anon_key(doc["anon_key"] | "");
        settings_save_supabase_api_key(doc["api_key"] | "");
        settings_save_supabase_device_key(doc["device_key"] | "");
        apply_settings_posthook("set_supabase");
        send_response("{\"ok\":true,\"msg\":\"supabase_saved\"}");
    } else if (strcmp(cmd, "get_supabase") == 0) {
        if (!check_pin(doc)) return;
        char url[128] = "", anon_key[128] = "", device_key[64] = "", api_key[64] = "";
        JsonDocument resp;
        bool has_url = settings_load_supabase_url(url, sizeof(url));
        bool has_akey = settings_load_supabase_anon_key(anon_key, sizeof(anon_key));
        bool has_dkey = settings_load_supabase_device_key(device_key, sizeof(device_key));
        bool has_apikey = settings_load_supabase_api_key(api_key, sizeof(api_key));
        if (has_url && has_akey && has_dkey && has_apikey) {
            resp["ok"] = true;
            resp["url"] = url;
            resp["anon_key"] = "***";
            resp["api_key"] = api_key;
            resp["device_key"] = device_key;
        } else {
            resp["ok"] = false;
            resp["error"] = "supabase_not_configured";
        }
        char buf[384];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "set_channel_group") == 0) {
        if (!check_pin(doc)) return;
        ChannelGroup cg = {};
        cg.group_id = doc["group_id"] | 0;
        strlcpy(cg.name, doc["name"] | "", sizeof(cg.name));
        cg.icon = doc["icon"] | 0;
        cg.channel_mask = doc["channel_mask"] | 0;
        settings_save_channel_group(cg.group_id, &cg);
        send_response("{\"ok\":true,\"msg\":\"group_saved\"}");
    } else if (strcmp(cmd, "get_channel_group") == 0) {
        if (!check_pin(doc)) return;
        uint8_t idx = doc["group_id"] | 0;
        ChannelGroup cg;
        JsonDocument resp;
        if (settings_load_channel_group(idx, &cg)) {
            resp["ok"] = true;
            resp["group_id"] = cg.group_id;
            resp["name"] = cg.name;
            resp["icon"] = cg.icon;
            resp["channel_mask"] = cg.channel_mask;
        } else {
            resp["ok"] = false;
            resp["error"] = "group_not_found";
        }
        char buf[256];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "set_battery_profile") == 0) {
        if (!check_pin(doc)) return;
        BatteryProfile bp = {};
        bp.channel = doc["channel"] | 0;
        strlcpy(bp.name, doc["name"] | "", sizeof(bp.name));
        bp.chemistry = doc["chemistry"] | 0;
        bp.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bp.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        bp.cell_count = doc["cell_count"] | 1.0f;
        bp.full_voltage = doc["full_voltage"] | 0.0f;
        bp.cutoff_voltage = doc["cutoff_voltage"] | 0.0f;
        bp.float_voltage = doc["float_voltage"] | 0.0f;
        settings_save_battery_profile(bp.channel, &bp);
        send_response("{\"ok\":true,\"msg\":\"battery_profile_saved\"}");
    } else if (strcmp(cmd, "get_battery_profile") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        BatteryProfile bp;
        JsonDocument resp;
        if (settings_load_battery_profile(ch, &bp)) {
            resp["ok"] = true;
            resp["channel"] = bp.channel;
            resp["name"] = bp.name;
            resp["chemistry"] = bp.chemistry;
            resp["capacity_mAh"] = bp.capacity_mAh;
            resp["initial_soc_pct"] = bp.initial_soc_pct;
            resp["cell_count"] = bp.cell_count;
            resp["full_voltage"] = bp.full_voltage;
            resp["cutoff_voltage"] = bp.cutoff_voltage;
            resp["float_voltage"] = bp.float_voltage;
        } else {
            resp["ok"] = false;
            resp["error"] = "battery_profile_not_found";
        }
        char buf[384];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "set_channel_name") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        settings_save_channel_name(ch, doc["name"] | "");
        send_response("{\"ok\":true,\"msg\":\"channel_name_saved\"}");
    } else if (strcmp(cmd, "get_channel_name") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        char name[24] = "";
        settings_load_channel_name(ch, name, sizeof(name));
        JsonDocument resp;
        resp["ok"] = true;
        resp["channel"] = ch;
        resp["name"] = name;
        char buf[128];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "set_calibration") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        uint8_t type = doc["type"] | 0;
        float value = doc["value"] | 0.0f;
        if (ch > 2) {
            send_response("{\"ok\":false,\"error\":\"invalid_channel\"}");
            return;
        }
        sensor_set_calibration(ch, type, value);
        sync_calibration_to_supabase();
        send_response("{\"ok\":true,\"msg\":\"calibration_saved\"}");
    } else if (strcmp(cmd, "get_calibration") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) {
            send_response("{\"ok\":false,\"error\":\"invalid_channel\"}");
            return;
        }
        float volt_offset_mv, volt_gain, curr_offset_ma, curr_gain;
        sensor_get_calibration(ch, &volt_offset_mv, &volt_gain, &curr_offset_ma, &curr_gain);
        JsonDocument resp;
        resp["ok"] = true;
        resp["channel"] = ch;
        resp["volt_offset_mv"] = volt_offset_mv;
        resp["volt_gain"] = volt_gain;
        resp["curr_offset_ma"] = curr_offset_ma;
        resp["curr_gain"] = curr_gain;
        char buf[256];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "reset_calibration") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) {
            send_response("{\"ok\":false,\"error\":\"invalid_channel\"}");
            return;
        }
        sensor_reset_calibration(ch);
        send_response("{\"ok\":true,\"msg\":\"calibration_reset\"}");
    } else if (strcmp(cmd, "set_virtual_channel") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 3) {
            send_response("{\"ok\":false,\"error\":\"invalid_channel\"}");
            return;
        }
        VirtualChannelConfig vc = {};
        vc.voltage_src = doc["voltage_src"] | 0;
        vc.voltage_idx = doc["voltage_idx"] | 0;
        vc.current_src = doc["current_src"] | 0;
        vc.current_idx = doc["current_idx"] | 0;
        settings_save_virtual_channel(ch, &vc);
        send_response("{\"ok\":true,\"msg\":\"virtual_channel_saved\"}");
    } else if (strcmp(cmd, "get_virtual_channel") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        VirtualChannelConfig vc;
        JsonDocument resp;
        if (settings_load_virtual_channel(ch, &vc)) {
            resp["ok"] = true;
            resp["channel"] = ch;
            resp["voltage_src"] = vc.voltage_src;
            resp["voltage_idx"] = vc.voltage_idx;
            resp["current_src"] = vc.current_src;
            resp["current_idx"] = vc.current_idx;
        } else {
            resp["ok"] = false;
            resp["error"] = "virtual_channel_not_found";
        }
        char buf[256];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "factory_reset") == 0) {
        if (!check_pin(doc)) return;
        settings_factory_reset();
        send_response("{\"ok\":true,\"msg\":\"factory_reset_done_reboot\"}");
    } else if (strcmp(cmd, "reboot") == 0) {
        if (!check_pin(doc)) return;
        send_response("{\"ok\":true,\"msg\":\"rebooting\"}");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
    } else {
        send_response("{\"ok\":false,\"error\":\"unknown_cmd\"}");
    }
}

// Apply a settings command from Supabase (no PIN check — Supabase auth is trusted)
// Reuses the same command dispatch logic from handle_command, but without PIN requirement.
// cmd_type: command name string (e.g. "set_wifi", "set_calibration")
// payload_json: JSON string containing the command parameters
void apply_settings_command(const char* cmd_type, const char* payload_json) {
    Serial.printf("[CMD] applying: %s\n", cmd_type);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload_json);
    if (err) { Serial.println("[CMD] bad json payload"); return; }

    if (strcmp(cmd_type, "set_wifi") == 0) {
        if (doc["ssid"] && doc["pass"]) {
            settings_save_wifi(doc["ssid"], doc["pass"]);
            Serial.println("[CMD] wifi saved");
        }
    } else if (strcmp(cmd_type, "set_mqtt") == 0) {
        if (doc["broker"]) {
            settings_save_mqtt(doc["broker"], doc["port"] | 1883, doc["topic"] | "");
            Serial.println("[CMD] mqtt saved");
        }
    } else if (strcmp(cmd_type, "set_http") == 0) {
        if (doc["url"]) {
            settings_save_http_endpoint(doc["url"], doc["token"] | "");
            settings_save_http_enabled(doc["enabled"] | true);
            Serial.println("[CMD] http saved");
        }
    } else if (strcmp(cmd_type, "set_supabase") == 0) {
        if (doc["url"]) {
            settings_save_supabase_url(doc["url"]);
            settings_save_supabase_anon_key(doc["anon_key"] | "");
            settings_save_supabase_api_key(doc["api_key"] | "");
            settings_save_supabase_device_key(doc["device_key"] | "");
            Serial.println("[CMD] supabase saved");
        }
    } else if (strcmp(cmd_type, "set_shunt") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_shunt(ch, doc["ohms"] | 0.0f);
        Serial.println("[CMD] shunt saved");
    } else if (strcmp(cmd_type, "set_volt_ratio") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_volt_ratio(ch, doc["ratio"] | 0.0f);
        Serial.println("[CMD] volt_ratio saved");
    } else if (strcmp(cmd_type, "set_resistors") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_resistors(ch, doc["r_high"] | 0.0f, doc["r_low"] | 0.0f);
        Serial.println("[CMD] resistors saved");
    } else if (strcmp(cmd_type, "set_relay") == 0) {
        RelayRule rt = {};
        rt.channel = doc["idx"] | 0;  // use idx as channel, not the hardcoded channel field from UI
        rt.overcurrent_A = doc["overcurrent_A"] | 0.0f;
        rt.undervoltage_V = doc["undervoltage_V"] | 0.0f;
        rt.soc_low_pct = doc["soc_low_pct"] | 0.0f;
        rt.soc_high_pct = doc["soc_high_pct"] | 100.0f;
        rt.trip_delay_ms = doc["trip_delay_ms"] | 1000;
        rt.reset_delay_ms = doc["reset_delay_ms"] | 5000;
        uint8_t default_relay_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
        rt.gpio_pin = default_relay_pins[rt.channel];  // always use board-specific default — don't accept gpio_pin from Supabase
        rt.active_high = doc["active_high"] | true;
        rt.enabled = doc["enabled"] | true;
        settings_save_relay(doc["idx"] | 0, &rt);
        Serial.println("[CMD] relay saved");
    } else if (strcmp(cmd_type, "set_battery") == 0) {
        BatteryConfig bat = {};
        bat.channel = doc["channel"] | 0;
        bat.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bat.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        settings_save_battery(bat.channel, &bat);
        Serial.println("[CMD] battery saved");
    } else if (strcmp(cmd_type, "set_battery_profile") == 0) {
        BatteryProfile bp = {};
        bp.channel = doc["channel"] | 0;
        strlcpy(bp.name, doc["name"] | "", sizeof(bp.name));
        {
            const char* chem = doc["chemistry"] | "";
            if (strcmp(chem, "lead_acid") == 0) bp.chemistry = 0;
            else if (strcmp(chem, "lipol") == 0) bp.chemistry = 1;
            else if (strcmp(chem, "liion") == 0) bp.chemistry = 2;
            else if (strcmp(chem, "nimh") == 0) bp.chemistry = 3;
            else if (strcmp(chem, "lifepo4") == 0) bp.chemistry = 4;
            else if (strcmp(chem, "agm") == 0) bp.chemistry = 5;
            else if (strcmp(chem, "fla") == 0) bp.chemistry = 6;
            else bp.chemistry = 0;
        }
        bp.system_voltage = doc["system_voltage"] | 0.0f;
        bp.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bp.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        bp.cell_count = doc["cell_count"] | 1.0f;
        bp.full_voltage = doc["full_voltage"] | 0.0f;
        bp.cutoff_voltage = doc["cutoff_voltage"] | 0.0f;
        bp.float_voltage = doc["float_voltage"] | 0.0f;
        settings_save_battery_profile(bp.channel, &bp);
        Serial.println("[CMD] battery_profile saved");
    } else if (strcmp(cmd_type, "set_calibration") == 0) {
        uint8_t ch = doc["channel"] | 0;
        uint8_t type = doc["type"] | 0;
        float value = doc["value"] | 0.0f;
        sensor_set_calibration(ch, type, value);
        Serial.println("[CMD] calibration saved");
    } else if (strcmp(cmd_type, "set_virtual_channel") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 3) return;
        VirtualChannelConfig vc = {};
        vc.voltage_src = doc["voltage_src"] | 0;
        vc.voltage_idx = doc["voltage_idx"] | 0;
        vc.current_src = doc["current_src"] | 0;
        vc.current_idx = doc["current_idx"] | 0;
        settings_save_virtual_channel(ch, &vc);
        Serial.println("[CMD] virtual_channel saved");
    } else if (strcmp(cmd_type, "set_channel_group") == 0) {
        ChannelGroup cg = {};
        cg.group_id = doc["group_id"] | 0;
        strlcpy(cg.name, doc["name"] | "", sizeof(cg.name));
        cg.icon = doc["icon"] | 0;
        cg.channel_mask = doc["channel_mask"] | 0;
        settings_save_channel_group(cg.group_id, &cg);
        Serial.println("[CMD] channel_group saved");
    } else if (strcmp(cmd_type, "set_channel_name") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_channel_name(ch, doc["name"] | "");
        Serial.println("[CMD] channel_name saved");
    } else if (strcmp(cmd_type, "set_pin") == 0) {
        settings_save_ble_pin(doc["new_pin"] | 0);
        sync_ble_pin_to_supabase();
        Serial.println("[CMD] ble_pin updated");
    } else if (strcmp(cmd_type, "calibrate_baseline") == 0) {
        sensor_calibrate_baseline();
        sync_calibration_to_supabase();
        Serial.println("[CMD] baseline calibration started");
    } else if (strcmp(cmd_type, "factory_reset") == 0) {
        settings_factory_reset();
        Serial.println("[CMD] factory_reset done — rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
    } else if (strcmp(cmd_type, "reboot") == 0) {
        Serial.println("[CMD] rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
    } else {
        Serial.printf("[CMD] unknown: %s\n", cmd_type);
    }
}

void init_ble_provisioner() {
    if (ble_initialized) return;
    NimBLEDevice::init(BT_DEVICE_NAME);
    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ProvServerCallbacks());
    pServer->advertiseOnDisconnect(true);  // NimBLE auto-restarts advertising on disconnect
    NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

    pCmdChar = pService->createCharacteristic(
        BLE_CHAR_CMD_UUID,
        NIMBLE_PROPERTY::WRITE
    );
    pCmdChar->setCallbacks(new CmdCallbacks());

    pRespChar = pService->createCharacteristic(
        BLE_CHAR_RESP_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pStatusChar = pService->createCharacteristic(
        BLE_CHAR_STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pSensorChar = pService->createCharacteristic(
        BLE_CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Configure advertising ONCE — addServiceUUID is cumulative; never repeat it
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->setName(BT_DEVICE_NAME);
    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    pAdvertising->enableScanResponse(true);

    ble_initialized = true;
    start_ble_advertising();
    Serial.println("BLE server ready");
}

void start_ble_advertising() {
    if (!ble_initialized || ble_advertising_active) return;
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (pAdvertising->start()) {
        ble_advertising_active = true;
        Serial.println("BLE advertising started");
    } else {
        Serial.println("BLE advertising start failed");
    }
}

void stop_ble_advertising() {
    if (!ble_initialized || !ble_advertising_active) return;
    NimBLEDevice::stopAdvertising();
    ble_advertising_active = false;
    Serial.println("BLE advertising stopped");
}

void deinit_ble_provisioner() {
    if (!ble_initialized) return;
    stop_ble_advertising();
    NimBLEDevice::deinit(true);
    ble_initialized = false;
    bleClientConnected = false;
    pCmdChar = nullptr;
    pRespChar = nullptr;
    pStatusChar = nullptr;
    pSensorChar = nullptr;
    Serial.printf("BLE deinit'd — freed ~50KB heap (free=%u)\n", ESP.getFreeHeap());
}

void loop_ble_provisioner() {
    if (!ble_initialized || !bleClientConnected || !pStatusChar) return;
    // Broadcast status every 2s to keep connection alive
    static unsigned long last_status = 0;
    if (millis() - last_status >= 2000) {
        last_status = millis();
        JsonDocument doc;
        doc["uptime_s"] = millis() / 1000;
        doc["entries"] = log_entries_count();
        doc["buffer_kb"] = log_buffer_capacity() / 1024;
        doc["overflow"] = log_has_overflow_file();
        char buf[128];
        serializeJson(doc, buf);
        pStatusChar->notify((const uint8_t*)buf, strlen(buf));
    }
}

void ble_notify_sensor_data(const char* data, size_t len) {
    if (ble_initialized && bleClientConnected && pSensorChar) {
        pSensorChar->notify((const uint8_t*)data, len);
    }
}
