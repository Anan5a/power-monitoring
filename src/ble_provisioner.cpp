#include "ble_provisioner.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "log_serial.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "sensor_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "switch_controller.h"
#include "connectivity_manager.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "cycle_counter.h"
#include "capacity_test.h"
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
        LOG_PRINT("[BLE] client connected (addr=%s)\n", connInfo.getAddress().toString().c_str());
        // Do NOT update connection params — let NimBLE use the defaults negotiated by the central.
        // Windows/Web Bluetooth often disconnects immediately when the peripheral overrides params.
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        bleClientConnected = false;
        // advertiseOnDisconnect(true) handles restart automatically
        LOG_PRINT("[BLE] client disconnected (reason=%d)\n", reason);
    }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        std::string val = pCharacteristic->getValue();
        LOG_PRINT("[BLE] onWrite len=%d\n", val.length());
        if (val.empty()) return;
        handle_command(val.c_str());
    }
};

static void send_response(const char* msg) {
    if (!bleClientConnected || !pRespChar) { LOG_PRINTLN(F("[BLE] send_response: not connected or pRespChar is null")); return; }
    size_t len = strlen(msg);
    pRespChar->notify((const uint8_t*)msg, len);
    LOG_PRINT("[BLE] sent resp: %s\n", msg);
}

// Centralized error responder — every command error MUST go through here so we
// (a) include a `cmd` echo (contract) and (b) log at debug level. The caller
// passes the cmd name it parsed (may be empty for parse-level errors).
static void send_error(const char* cmd, const char* err) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\",\"cmd\":\"%s\"}",
             err, (cmd && *cmd) ? cmd : "");
    send_response(buf);
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
            LOG_PRINTLN(F("[BLE] rate limited"));
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
    LOG_PRINT("[BLE] command: %s\n", json);
    if (!check_rate_limit()) { LOG_PRINTLN(F("[BLE] rate limited")); return; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { LOG_PRINTLN(F("[BLE] bad json")); send_response("{\"ok\":false,\"error\":\"bad_json\"}"); return; }

    const char* cmd = doc["cmd"] | "";
    LOG_PRINT("[BLE] cmd: %s\n", cmd);
    if (strcmp(cmd, "set_wifi") == 0) {
        uint32_t pin = doc["pin"] | 0;
        LOG_PRINT("[BLE] set_wifi pin=%lu\n", pin);
        uint32_t stored_pin = settings_load_ble_pin();
        LOG_PRINT("[BLE] stored_pin=%lu expected=%lu\n", stored_pin, pin);
        if (!check_pin(doc)) { LOG_PRINTLN("[BLE] pin check failed"); return; }
        settings_save_wifi(doc["ssid"], doc["pass"]);
        apply_settings_posthook("set_wifi");
        send_response("{\"ok\":true,\"msg\":\"wifi_saved_reconnecting\"}");
        LOG_PRINTLN("[BLE] wifi saved");
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
    } else if (strcmp(cmd, "set_switch") == 0 || strcmp(cmd, "set_relay") == 0) {
        if (!check_pin(doc)) return;
        uint8_t idx = doc["idx"] | 0;
        // switch_controller supports up to 8 switches (MAX_SWITCHES).
        if (idx > 7) { send_error(cmd, "invalid_idx"); return; }
        uint8_t default_switch_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
        SwitchChannel ch = {};
        ch.idx = idx;
        ch.type = doc["type"] | SW_RELAY;
        ch.gpio_pin = doc["gpio_pin"] | default_switch_pins[idx];
        ch.active_high = doc["active_high"] | true;
        ch.enabled = doc["enabled"] | true;
        ch.is_energized = get_switch_state(idx);
        snprintf(ch.name, sizeof(ch.name), "Switch %u", (unsigned)idx);
        settings_save_switch(idx, &ch);

        // New list-shape: { conditions:[{kind,op,value,ref_channel,schedule_mask}],
        //                    logic:"AND"|"OR", min_conditions:N,
        //                    trip_delay_ms, reset_delay_ms, hysteresis, rule_enabled }
        // Legacy flat-shape: { channel, overcurrent_A, undervoltage_V, soc_low/high, ... }
        // We always store the new shape. If legacy fields are present, we
        // synthesise a single OR-rule from them so older dashboards still work.
        SwitchRule rule = {};
        rule.switch_idx     = idx;
        rule.channel        = doc["channel"] | idx;
        rule.trip_delay_ms  = doc["trip_delay_ms"]  | 1000;
        rule.reset_delay_ms = doc["reset_delay_ms"] | 5000;
        rule.logic          = SL_OR;
        rule.min_conditions = doc["min_conditions"] | 0;
        rule.enabled        = doc["rule_enabled"]   | true;
        rule.hysteresis     = doc["hysteresis"]     | 0.0f;
        if (doc["logic"].is<const char*>()) {
            if (strcmp(doc["logic"].as<const char*>(), "AND") == 0) rule.logic = SL_AND;
        }

        if (JsonArray conds = doc["conditions"].as<JsonArray>()) {
            uint8_t n = 0;
            for (JsonObject co : conds) {
                if (n >= SC_MAX_CONDITIONS) break;
                SwitchCondition& c = rule.conditions[n];
                memset(&c, 0, sizeof(c));
                const char* kind = co["kind"] | "OVERCURRENT";
                if      (strcmp(kind, "OVERCURRENT")     == 0) c.kind = SCK_OVERCURRENT;
                else if (strcmp(kind, "UNDERVOLTAGE")    == 0) c.kind = SCK_UNDERVOLTAGE;
                else if (strcmp(kind, "SOC_LOW")         == 0) c.kind = SCK_SOC_LOW;
                else if (strcmp(kind, "SOC_HIGH")        == 0) c.kind = SCK_SOC_HIGH;
                else if (strcmp(kind, "CHANNEL_ABOVE")   == 0) c.kind = SCK_CHANNEL_ABOVE;
                else if (strcmp(kind, "CHANNEL_BELOW")   == 0) c.kind = SCK_CHANNEL_BELOW;
                else if (strcmp(kind, "SCHEDULE_WINDOW") == 0) c.kind = SCK_SCHEDULE_WINDOW;
                else                                            c.kind = SCK_DISABLED;

                const char* op = co["op"] | ">";
                if      (strcmp(op, ">")  == 0) c.op = SCO_GT;
                else if (strcmp(op, "<")  == 0) c.op = SCO_LT;
                else if (strcmp(op, ">=") == 0) c.op = SCO_GTE;
                else if (strcmp(op, "<=") == 0) c.op = SCO_LTE;
                else if (strcmp(op, "==") == 0) c.op = SCO_EQ;
                else                                c.op = SCO_GT;

                c.value       = co["value"] | 0.0f;
                c.ref_channel = (int8_t)(co["ref_channel"] | -1);
                JsonArray mask = co["schedule_mask"].as<JsonArray>();
                if (mask) {
                    for (uint8_t bi = 0; bi < SC_SCHEDULE_BYTES && bi < mask.size(); bi++) {
                        c.schedule_mask[bi] = mask[bi].as<uint8_t>();
                    }
                }
                n++;
            }
            rule.condition_count = n;
        } else {
            // Legacy flat-shape fallback: synthesise one OR condition per non-zero field.
            uint8_t n = 0;
            float overA = doc["overcurrent_A"]  | 0.0f;
            float undV  = doc["undervoltage_V"] | 0.0f;
            float socLo = doc["soc_low_pct"]    | 0.0f;
            float socHi = doc["soc_high_pct"]   | 0.0f;
            if (overA > 1e-6f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_OVERCURRENT; c.op = SCO_GT; c.ref_channel = -1; c.value = overA;
            }
            if (undV > 1e-6f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_UNDERVOLTAGE; c.op = SCO_LT; c.ref_channel = -1; c.value = undV;
            }
            if (socLo > 1e-3f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_SOC_LOW; c.op = SCO_LT; c.ref_channel = -1; c.value = socLo;
            }
            if (socHi > 1e-3f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_SOC_HIGH; c.op = SCO_GT; c.ref_channel = -1; c.value = socHi;
            }
            rule.condition_count = n;
        }
        settings_save_switch_rule(idx, &rule);

        publish_switch_state(idx, ch.is_energized);
        send_response("{\"ok\":true,\"msg\":\"switch_saved\"}");
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
        uint32_t new_pin = doc["new_pin"] | 0;
        // 0 = clear security, 1..999999 = valid 6-digit PINs.
        if (new_pin > 999999) { send_error(cmd, "invalid_value"); return; }
        settings_save_ble_pin(new_pin);
        sync_ble_pin_to_supabase();
        send_response("{\"ok\":true,\"msg\":\"pin_updated\"}");
    } else if (strcmp(cmd, "get_status") == 0) {
        JsonDocument resp;
        resp["ok"] = true;
        resp["entries"] = log_entries_count();
        resp["buffer_kb"] = log_buffer_capacity() / 1024;
        resp["overflow"] = log_has_overflow_file();
        resp["switch_count"] = settings_load_switch_count();
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
    } else if (strcmp(cmd, "discover_sensors") == 0) {
        if (!check_pin(doc)) return;
        discover_sensors();
        send_response("{\"ok\":true,\"msg\":\"discovery_complete\"}");
    } else if (strcmp(cmd, "get_switch") == 0 || strcmp(cmd, "get_relay") == 0) {
        if (!check_pin(doc)) return;
        uint8_t idx = doc["idx"] | 0;
        SwitchChannel ch;
        SwitchRule rule;
        JsonDocument resp;
        if (settings_load_switch(idx, &ch) && settings_load_switch_rule(idx, &rule)) {
            resp["ok"] = true;
            resp["idx"] = idx;
            resp["type"] = ch.type;
            resp["gpio_pin"] = ch.gpio_pin;
            resp["active_high"] = ch.active_high;
            resp["enabled"] = ch.enabled;
            resp["is_energized"] = ch.is_energized;
            resp["channel"] = rule.channel;
            resp["trip_delay_ms"] = rule.trip_delay_ms;
            resp["reset_delay_ms"] = rule.reset_delay_ms;
            resp["logic"] = (rule.logic == SL_AND) ? "AND" : "OR";
            resp["min_conditions"] = rule.min_conditions;
            resp["rule_enabled"] = rule.enabled;
            resp["hysteresis"] = rule.hysteresis;
            resp["condition_count"] = rule.condition_count;
            JsonArray arr = resp["conditions"].to<JsonArray>();
            for (uint8_t i = 0; i < rule.condition_count && i < SC_MAX_CONDITIONS; i++) {
                const SwitchCondition& c = rule.conditions[i];
                JsonObject o = arr.add<JsonObject>();
                o["kind"] = switch_condition_kind_name(c.kind);
                o["op"]   = switch_condition_op_name(c.op);
                o["value"] = c.value;
                o["ref_channel"] = c.ref_channel;
                if (c.kind == SCK_SCHEDULE_WINDOW) {
                    JsonArray mask = o["schedule_mask"].to<JsonArray>();
                    for (uint8_t bi = 0; bi < SC_SCHEDULE_BYTES; bi++) {
                        mask.add(c.schedule_mask[bi]);
                    }
                }
            }
        } else {
            resp["ok"] = false;
            resp["error"] = "switch_not_found";
        }
        char buf[768];
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
            send_error(cmd, "invalid_channel");
            return;
        }
        sensor_set_calibration(ch, type, value);
        sync_calibration_to_supabase();
        send_response("{\"ok\":true,\"msg\":\"calibration_saved\"}");
    } else if (strcmp(cmd, "get_calibration") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) {
            send_error(cmd, "invalid_channel");
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
            send_error(cmd, "invalid_channel");
            return;
        }
        sensor_reset_calibration(ch);
        send_response("{\"ok\":true,\"msg\":\"calibration_reset\"}");
    } else if (strcmp(cmd, "set_invert_curr") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) {
            send_error(cmd, "invalid_channel");
            return;
        }
        sensor_set_invert_curr(ch, doc["invert"] | false);
        send_response("{\"ok\":true,\"msg\":\"invert_curr_saved\"}");
    } else if (strcmp(cmd, "get_invert_curr") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) {
            send_error(cmd, "invalid_channel");
            return;
        }
        ChannelCalibration cal;
        settings_load_channel_calibration(&cal);
        JsonDocument resp;
        resp["ok"] = true;
        resp["channel"] = ch;
        resp["invert_curr"] = cal.invert_curr[ch];
        char buf[128];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "reset_invert_curr") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) {
            send_error(cmd, "invalid_channel");
            return;
        }
        sensor_reset_invert_curr(ch);
        send_response("{\"ok\":true,\"msg\":\"invert_curr_reset\"}");
    } else if (strcmp(cmd, "set_virtual_channel") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (ch > 3) {
            send_error(cmd, "invalid_channel");
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
    } else if (strcmp(cmd, "list_battery_profiles") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ids[BATTERY_MAX_PROFILES];
        uint8_t n = battery_profile_list_ids(ids, BATTERY_MAX_PROFILES);
        JsonDocument resp;
        resp["ok"] = true;
        JsonArray arr = resp["profiles"].to<JsonArray>();
        for (uint8_t i = 0; i < n; i++) {
            const BatteryChemistryProfile* p = battery_profile_get(ids[i]);
            if (!p) continue;
            JsonObject o = arr.add<JsonObject>();
            o["id"] = p->id;
            o["name"] = p->name;
            o["chemistry"] = p->chemistry;
            o["chemistry_name"] = battery_chemistry_name(p->chemistry);
            o["nominal_voltage"] = p->nominal_voltage;
            o["rated_capacity_Ah"] = p->rated_capacity_Ah;
            o["cutoff_voltage"] = p->cutoff_voltage;
            o["float_voltage"] = p->float_voltage;
        }
        char buf[1024];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "get_battery_profile") == 0) {
        if (!check_pin(doc)) return;
        uint8_t id = doc["id"] | 0;
        const BatteryChemistryProfile* p = battery_profile_get(id);
        JsonDocument resp;
        if (!p) {
            resp["ok"] = false;
            resp["error"] = "profile_not_found";
        } else {
            resp["ok"] = true;
            resp["id"] = p->id;
            resp["name"] = p->name;
            resp["chemistry"] = p->chemistry;
            resp["chemistry_name"] = battery_chemistry_name(p->chemistry);
            resp["nominal_voltage"] = p->nominal_voltage;
            resp["rated_capacity_Ah"] = p->rated_capacity_Ah;
            resp["c_rating"] = p->c_rating;
            resp["cutoff_voltage"] = p->cutoff_voltage;
            resp["float_voltage"] = p->float_voltage;
            resp["charge_efficiency"] = p->charge_efficiency;
            resp["cycle_life_rated"] = p->cycle_life_rated;
            resp["min_soc_pct"] = p->min_soc_pct;
            resp["max_soc_pct"] = p->max_soc_pct;
        }
        char buf[512];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "set_battery_profile") == 0) {
        if (!check_pin(doc)) return;
        uint8_t id = doc["id"] | 0;
        if (id >= BATTERY_MAX_PROFILES) {
            send_error(cmd, "invalid_id");
            return;
        }
        BatteryChemistryProfile p = {};
        const BatteryChemistryProfile* existing = battery_profile_get(id);
        if (existing) p = *existing;
        p.id = id;
        if (doc["name"].is<const char*>()) strlcpy(p.name, doc["name"] | "", sizeof(p.name));
        if (doc["chemistry"].is<int>()) p.chemistry = doc["chemistry"] | p.chemistry;
        p.nominal_voltage = doc["nominal_voltage"] | p.nominal_voltage;
        p.rated_capacity_Ah = doc["rated_capacity_Ah"] | p.rated_capacity_Ah;
        p.c_rating = doc["c_rating"] | p.c_rating;
        p.cutoff_voltage = doc["cutoff_voltage"] | p.cutoff_voltage;
        p.float_voltage = doc["float_voltage"] | p.float_voltage;
        p.charge_efficiency = doc["charge_efficiency"] | p.charge_efficiency;
        p.cycle_life_rated = doc["cycle_life_rated"] | p.cycle_life_rated;
        p.min_soc_pct = doc["min_soc_pct"] | p.min_soc_pct;
        p.max_soc_pct = doc["max_soc_pct"] | p.max_soc_pct;
        if (!battery_profile_set(&p)) {
            send_response("{\"ok\":false,\"error\":\"set_failed\"}");
            return;
        }
        send_response("{\"ok\":true,\"msg\":\"profile_saved\"}");
    } else if (strcmp(cmd, "delete_battery_profile") == 0) {
        if (!check_pin(doc)) return;
        uint8_t id = doc["id"] | 0;
        if (!battery_profile_delete(id)) {
            send_response("{\"ok\":false,\"error\":\"delete_failed\"}");
            return;
        }
        send_response("{\"ok\":true,\"msg\":\"profile_deleted\"}");
    } else if (strcmp(cmd, "get_battery") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        uint8_t pid = battery_channel_profile(ch);
        JsonDocument resp;
        resp["ok"] = true;
        resp["channel"] = ch;
        resp["profile_id"] = (pid == BATTERY_CHANNEL_NO_BINDING) ? -1 : (int)pid;
        BatteryState st;
        cycle_counter_get(ch, &st);
        resp["cumulative_Ah_in"] = st.cumulative_Ah_in;
        resp["cumulative_Ah_out"] = st.cumulative_Ah_out;
        resp["equivalent_full_cycles"] = st.equivalent_full_cycles;
        resp["last_SoC_pct"] = st.last_SoC_pct;
        resp["last_V"] = st.last_V;
        resp["last_I"] = st.last_I;
        resp["last_update_ms"] = st.last_update_ms;
        char buf[384];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "set_battery") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        int pid_in = doc["profile_id"] | -1;
        uint8_t pid;
        if (pid_in < 0) {
            battery_channel_clear(ch);
            pid = BATTERY_CHANNEL_NO_BINDING;
        } else {
            pid = (uint8_t)pid_in;
            if (!battery_channel_set_profile(ch, pid)) {
                send_response("{\"ok\":false,\"error\":\"set_failed\"}");
                return;
            }
        }
        JsonDocument resp;
        resp["ok"] = true;
        resp["channel"] = ch;
        resp["profile_id"] = (pid == BATTERY_CHANNEL_NO_BINDING) ? -1 : (int)pid;
        char buf[128];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "reset_battery") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        cycle_counter_reset(ch);
        send_response("{\"ok\":true,\"msg\":\"battery_reset\"}");
    } else if (strcmp(cmd, "get_cycle_state") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        BatteryState st;
        cycle_counter_get(ch, &st);
        JsonDocument resp;
        resp["ok"] = true;
        resp["channel"] = ch;
        resp["cumulative_Ah_in"] = st.cumulative_Ah_in;
        resp["cumulative_Ah_out"] = st.cumulative_Ah_out;
        resp["equivalent_full_cycles"] = st.equivalent_full_cycles;
        resp["current_session_dod_Ah"] = st.current_session_dod_Ah;
        resp["last_session_start_pct"] = st.last_session_start_pct;
        resp["last_SoC_pct"] = st.last_SoC_pct;
        char buf[256];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "capacity_test") == 0) {
        if (!check_pin(doc)) return;
        const char* action = doc["action"] | "";
        uint8_t ch = doc["channel"] | 0;
        if (strcmp(action, "start") == 0) {
            uint8_t mode = doc["mode"] | 0;
            int8_t lsi = doc["load_switch_idx"] | -1;
            float cutoff = doc["cutoff_v"] | 0.0f;
            if (!capacity_test_start(ch, mode, lsi, cutoff)) {
                send_response("{\"ok\":false,\"error\":\"start_failed\"}");
                return;
            }
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"msg\":\"capacity_test_started\",\"channel\":%u,\"mode\":%u}",
                (unsigned)ch, (unsigned)mode);
            send_response(buf);
        } else if (strcmp(action, "stop") == 0) {
            CapacityTestResult r = capacity_test_stop(ch);
            if (!r.valid) {
                send_response("{\"ok\":false,\"error\":\"not_running\"}");
                return;
            }
            JsonDocument resp;
            resp["ok"] = true;
            resp["msg"] = "capacity_test_stopped";
            resp["channel"] = r.channel;
            resp["mode"] = r.mode;
            resp["measured_Ah"] = r.measured_Ah;
            resp["rated_Ah"] = r.rated_Ah;
            resp["soh_pct"] = r.soh_pct;
            resp["duration_s"] = r.duration_s;
            resp["samples"] = r.samples;
            resp["start_SoC_pct"] = r.start_SoC_pct;
            resp["end_SoC_pct"] = r.end_SoC_pct;
            char buf[384];
            serializeJson(resp, buf);
            send_response(buf);
        } else if (strcmp(action, "status") == 0) {
            BatteryState st;
            cycle_counter_get(ch, &st);
            JsonDocument resp;
            resp["ok"] = true;
            resp["channel"] = ch;
            resp["active"] = st.test.active;
            resp["mode"] = st.test.mode;
            resp["measured_Ah"] = st.test.measured_Ah;
            resp["start_SoC_pct"] = st.test.start_SoC_pct;
            resp["cutoff_v"] = st.test.cutoff_v;
            resp["load_switch_idx"] = st.test.load_switch_idx;
            char buf[256];
            serializeJson(resp, buf);
            send_response(buf);
        } else {
            send_error(cmd, "invalid_action");
        }
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
        // DEBUG-level log so unrecognised commands are visible in
        // CORE_DEBUG_LEVEL=3 builds but don't spam release logs.
#if CORE_DEBUG_LEVEL >= 3
        LOG_PRINT("[BLE] unknown cmd: \"%s\"\n", cmd);
#endif
        send_error(cmd, "unknown_cmd");
    }
}

// Apply a settings command from Supabase (no PIN check — Supabase auth is trusted)
// Reuses the same command dispatch logic from handle_command, but without PIN requirement.
// cmd_type: command name string (e.g. "set_wifi", "set_calibration")
// payload_json: JSON string containing the command parameters
void apply_settings_command(const char* cmd_type, const char* payload_json) {
    LOG_PRINT("[CMD] applying: %s\n", cmd_type);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload_json);
    if (err) { LOG_PRINTLN("[CMD] bad json payload"); return; }

    if (strcmp(cmd_type, "set_wifi") == 0) {
        if (doc["ssid"] && doc["pass"]) {
            settings_save_wifi(doc["ssid"], doc["pass"]);
            LOG_PRINTLN("[CMD] wifi saved");
        }
    } else if (strcmp(cmd_type, "set_mqtt") == 0) {
        if (doc["broker"]) {
            settings_save_mqtt(doc["broker"], doc["port"] | 1883, doc["topic"] | "");
            LOG_PRINTLN("[CMD] mqtt saved");
        }
    } else if (strcmp(cmd_type, "set_http") == 0) {
        if (doc["url"]) {
            settings_save_http_endpoint(doc["url"], doc["token"] | "");
            settings_save_http_enabled(doc["enabled"] | true);
            LOG_PRINTLN("[CMD] http saved");
        }
    } else if (strcmp(cmd_type, "set_supabase") == 0) {
        if (doc["url"]) {
            settings_save_supabase_url(doc["url"]);
            settings_save_supabase_anon_key(doc["anon_key"] | "");
            settings_save_supabase_api_key(doc["api_key"] | "");
            settings_save_supabase_device_key(doc["device_key"] | "");
            LOG_PRINTLN("[CMD] supabase saved");
        }
    } else if (strcmp(cmd_type, "set_shunt") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_shunt(ch, doc["ohms"] | 0.0f);
        LOG_PRINTLN("[CMD] shunt saved");
    } else if (strcmp(cmd_type, "set_volt_ratio") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_volt_ratio(ch, doc["ratio"] | 0.0f);
        LOG_PRINTLN("[CMD] volt_ratio saved");
    } else if (strcmp(cmd_type, "set_resistors") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_resistors(ch, doc["r_high"] | 0.0f, doc["r_low"] | 0.0f);
        LOG_PRINTLN("[CMD] resistors saved");
    } else if (strcmp(cmd_type, "set_switch") == 0 || strcmp(cmd_type, "set_relay") == 0) {
        uint8_t idx = doc["idx"] | 0;
        uint8_t default_switch_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
        SwitchChannel ch = {};
        ch.idx = idx;
        ch.type = doc["type"] | SW_RELAY;
        ch.gpio_pin = default_switch_pins[idx];
        ch.active_high = doc["active_high"] | true;
        ch.enabled = doc["enabled"] | true;
        snprintf(ch.name, sizeof(ch.name), "Switch %u", (unsigned)idx);
        settings_save_switch(idx, &ch);

        // Mirror BLE set_switch: list-shape preferred, legacy flat-shape synthesised.
        SwitchRule rule = {};
        rule.switch_idx     = idx;
        rule.channel        = doc["channel"] | 0;
        rule.trip_delay_ms  = doc["trip_delay_ms"]  | 1000;
        rule.reset_delay_ms = doc["reset_delay_ms"] | 5000;
        rule.logic          = SL_OR;
        rule.min_conditions = doc["min_conditions"] | 0;
        rule.enabled        = doc["enabled"] | true;
        rule.hysteresis     = doc["hysteresis"] | 0.0f;
        if (doc["logic"].is<const char*>()) {
            if (strcmp(doc["logic"].as<const char*>(), "AND") == 0) rule.logic = SL_AND;
        }
        if (JsonArray conds = doc["conditions"].as<JsonArray>()) {
            uint8_t n = 0;
            for (JsonObject co : conds) {
                if (n >= SC_MAX_CONDITIONS) break;
                SwitchCondition& c = rule.conditions[n];
                memset(&c, 0, sizeof(c));
                const char* kind = co["kind"] | "OVERCURRENT";
                if      (strcmp(kind, "OVERCURRENT")     == 0) c.kind = SCK_OVERCURRENT;
                else if (strcmp(kind, "UNDERVOLTAGE")    == 0) c.kind = SCK_UNDERVOLTAGE;
                else if (strcmp(kind, "SOC_LOW")         == 0) c.kind = SCK_SOC_LOW;
                else if (strcmp(kind, "SOC_HIGH")        == 0) c.kind = SCK_SOC_HIGH;
                else if (strcmp(kind, "CHANNEL_ABOVE")   == 0) c.kind = SCK_CHANNEL_ABOVE;
                else if (strcmp(kind, "CHANNEL_BELOW")   == 0) c.kind = SCK_CHANNEL_BELOW;
                else if (strcmp(kind, "SCHEDULE_WINDOW") == 0) c.kind = SCK_SCHEDULE_WINDOW;
                c.value = co["value"] | 0.0f;
                c.ref_channel = (int8_t)(co["ref_channel"] | -1);
                n++;
            }
            rule.condition_count = n;
        } else {
            uint8_t n = 0;
            float overA = doc["overcurrent_A"]  | 0.0f;
            float undV  = doc["undervoltage_V"] | 0.0f;
            float socLo = doc["soc_low_pct"]    | 0.0f;
            float socHi = doc["soc_high_pct"]   | 100.0f;
            if (overA > 1e-6f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_OVERCURRENT; c.op = SCO_GT; c.ref_channel = -1; c.value = overA;
            }
            if (undV > 1e-6f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_UNDERVOLTAGE; c.op = SCO_LT; c.ref_channel = -1; c.value = undV;
            }
            if (socLo > 1e-3f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_SOC_LOW; c.op = SCO_LT; c.ref_channel = -1; c.value = socLo;
            }
            if (socHi > 1e-3f && n < SC_MAX_CONDITIONS) {
                SwitchCondition& c = rule.conditions[n++];
                c.kind = SCK_SOC_HIGH; c.op = SCO_GT; c.ref_channel = -1; c.value = socHi;
            }
            rule.condition_count = n;
        }
        settings_save_switch_rule(idx, &rule);
        LOG_PRINTLN("[CMD] switch saved");
    } else if (strcmp(cmd_type, "set_battery") == 0) {
        BatteryConfig bat = {};
        bat.channel = doc["channel"] | 0;
        bat.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bat.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        settings_save_battery(bat.channel, &bat);
        LOG_PRINTLN("[CMD] battery saved");
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
        LOG_PRINTLN("[CMD] battery_profile saved");
    } else if (strcmp(cmd_type, "set_calibration") == 0) {
        uint8_t ch = doc["channel"] | 0;
        uint8_t type = doc["type"] | 0;
        float value = doc["value"] | 0.0f;
        sensor_set_calibration(ch, type, value);
        LOG_PRINTLN("[CMD] calibration saved");
    } else if (strcmp(cmd_type, "set_invert_curr") == 0) {
        uint8_t ch = doc["channel"] | 0;
        sensor_set_invert_curr(ch, doc["invert"] | false);
        LOG_PRINTLN("[CMD] invert_curr saved");
    } else if (strcmp(cmd_type, "reset_coulomb") == 0) {
        uint8_t ch = doc["channel"] | 0;
        reset_coulomb_counter(ch);
        LOG_PRINTLN("[CMD] coulomb_reset done");
    } else if (strcmp(cmd_type, "set_virtual_channel") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 3) return;
        VirtualChannelConfig vc = {};
        vc.voltage_src = doc["voltage_src"] | 0;
        vc.voltage_idx = doc["voltage_idx"] | 0;
        vc.current_src = doc["current_src"] | 0;
        vc.current_idx = doc["current_idx"] | 0;
        settings_save_virtual_channel(ch, &vc);
        LOG_PRINTLN("[CMD] virtual_channel saved");
    } else if (strcmp(cmd_type, "set_channel_group") == 0) {
        ChannelGroup cg = {};
        cg.group_id = doc["group_id"] | 0;
        strlcpy(cg.name, doc["name"] | "", sizeof(cg.name));
        cg.icon = doc["icon"] | 0;
        cg.channel_mask = doc["channel_mask"] | 0;
        settings_save_channel_group(cg.group_id, &cg);
        LOG_PRINTLN("[CMD] channel_group saved");
    } else if (strcmp(cmd_type, "set_channel_name") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_channel_name(ch, doc["name"] | "");
        LOG_PRINTLN("[CMD] channel_name saved");
    } else if (strcmp(cmd_type, "set_pin") == 0) {
        settings_save_ble_pin(doc["new_pin"] | 0);
        sync_ble_pin_to_supabase();
        LOG_PRINTLN("[CMD] ble_pin updated");
    } else if (strcmp(cmd_type, "calibrate_baseline") == 0) {
        sensor_calibrate_baseline();
        sync_calibration_to_supabase();
        LOG_PRINTLN("[CMD] baseline calibration started");
    } else if (strcmp(cmd_type, "discover_sensors") == 0) {
        discover_sensors();
        LOG_PRINTLN("[CMD] sensor discovery complete");
    } else if (strcmp(cmd_type, "factory_reset") == 0) {
        settings_factory_reset();
        LOG_PRINTLN("[CMD] factory_reset done — rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
    } else if (strcmp(cmd_type, "reboot") == 0) {
        LOG_PRINTLN("[CMD] rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
    } else {
        LOG_PRINT("[CMD] unknown: %s\n", cmd_type);
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
    LOG_PRINTLN("BLE server ready");
}

void start_ble_advertising() {
    if (!ble_initialized || ble_advertising_active) return;
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    if (pAdvertising->start()) {
        ble_advertising_active = true;
        LOG_PRINTLN("BLE advertising started");
    } else {
        LOG_PRINTLN("BLE advertising start failed");
    }
}

void stop_ble_advertising() {
    if (!ble_initialized || !ble_advertising_active) return;
    NimBLEDevice::stopAdvertising();
    ble_advertising_active = false;
    LOG_PRINTLN("BLE advertising stopped");
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
    LOG_PRINT("BLE deinit'd — freed ~50KB heap (free=%u)\n", ESP.getFreeHeap());
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
