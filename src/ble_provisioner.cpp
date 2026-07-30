#include "ble_provisioner.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "config.h"
#include "log_serial.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "sensor_manager.h"
#include "data_logger.h"
#include "device_state.h"
#include "device_identity.h"
#include "event_log.h"
#include "coulomb_counter.h"
#include "switch_controller.h"
#include "connectivity_manager.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "cycle_counter.h"
#include "ota_client.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

static NimBLECharacteristic* pCmdChar = nullptr;
static NimBLECharacteristic* pRespChar = nullptr;
static NimBLECharacteristic* pStatusChar = nullptr;
static NimBLECharacteristic* pSensorChar = nullptr;
static bool bleClientConnected = false;
static bool ble_initialized = false;

// Decouples onWrite (NimBLE host task) from handle_command (network task).
// Each entry is a NUL-terminated command JSON up to BLE_CMD_BUF_SIZE-1 bytes.
#define BLE_CMD_BUF_SIZE 1025
static QueueHandle_t ble_cmd_queue = nullptr;

// Rate limiting: track commands per connection window
#define RATE_WINDOW_MS    10000   // 10-second window
#define MAX_COMMANDS      10     // max 10 commands per window per connection
static uint16_t rate_cmd_count = 0;
static unsigned long rate_window_start = 0;
static unsigned long rate_last_cmd = 0;

static void handle_command(const char* json);
static void send_response(const char* msg);
static void send_error(const char* cmd, const char* err);

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
        if (val.empty()) return;
        // Reject oversized writes: 1024 is well over any reasonable command
        // JSON (largest legitimate command is ~600 bytes) and stops a bad
        // client from holding RAM during JSON parse + handler dispatch.
        if (val.length() > 1024) {
            LOG_PRINT("[BLE] rejecting oversized write: %u bytes\n", (unsigned)val.length());
            send_response("{\"ok\":false,\"error\":\"payload_too_large\"}");
            return;
        }
        // Enqueue for the network task (loop_ble_provisioner) instead of
        // running handle_command here on the NimBLE host task. The host task
        // must return quickly; NVS writes / WiFi reconnects / sensor cal
        // would otherwise block it and drop the BLE connection.
        char buf[BLE_CMD_BUF_SIZE];
        size_t n = val.copy(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        if (!ble_cmd_queue || xQueueSend(ble_cmd_queue, buf, 0) != pdTRUE) {
            send_response("{\"ok\":false,\"error\":\"server_busy\"}");
        }
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
// Validate a logical channel index. Returns true if 0 <= ch < MAX_LOGICAL_CHANNELS.
// Handlers that accept a "channel" parameter MUST call this before using the value
// to index into channel arrays, preventing out-of-bounds reads/writes.
static bool valid_channel(uint8_t ch) {
    return ch < MAX_LOGICAL_CHANNELS;
}

static void send_error(const char* cmd, const char* err) {
    // Build the error doc via ArduinoJson so that err/cmd strings containing
    // special characters (quotes, backslashes, etc.) are properly escaped.
    // The old snprintf approach could produce invalid JSON if an error string
    // contained a double-quote character.
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = err;
    if (cmd && *cmd) doc["cmd"] = cmd;
    char buf[192];
    serializeJson(doc, buf);
    send_response(buf);
}

static void send_ok(const char* cmd, const char* msg) {
    JsonDocument doc;
    doc["ok"] = true;
    doc["cmd"] = cmd;
    doc["msg"] = msg;
    char buf[192];
    serializeJson(doc, buf);
    send_response(buf);
}

// Brute-force protection: a persistent failed-PIN counter in NVS plus an
// in-session exponential backoff. The old burst limiter only counted commands
// within a 100 ms window, so one command every ~100 ms never tripped it —
// ~10 PIN guesses/sec with no backoff and no persistent lockout.
static unsigned long ble_last_fail_ms = 0;

static bool check_pin(JsonDocument& doc) {
    uint32_t expected = settings_load_ble_pin();
    if (expected == 0) {
        // No PIN configured — refuse all commands. The user must call
        // set_pin first (which does NOT go through check_pin) to establish
        // a PIN before any other command is accepted. This prevents an
        // unconfigured device from being controlled by anyone who can reach
        // its BLE advertising.
        return false;
    }
    uint16_t fails = settings_load_ble_fail_count();
    // Required cool-off grows exponentially with the persistent fail count,
    // capped at 1 hour. Even after a reboot (which resets the in-session
    // timer) the high fail count forces a long first cool-off.
    unsigned long cooloff_ms = 1000UL;
    if (fails > 0) {
        unsigned long unit = 1000UL << (fails > 12 ? 12 : fails); // 1s, 2s, 4s, ...
        cooloff_ms = (unit > 3600000UL) ? 3600000UL : unit;
    }
    if (fails > 0 && (millis() - ble_last_fail_ms) < cooloff_ms) {
        send_response("{\"ok\":false,\"error\":\"rate_limited\"}");
        return false;
    }
    // Accept PIN as number or as string (dashboard sends "123456" as JSON string)
    uint32_t provided = 0;
    if (doc["pin"].is<const char*>()) {
        provided = atoi(doc["pin"].as<const char*>());
    } else {
        provided = doc["pin"] | 0;
    }
    if (provided != expected) {
        ble_last_fail_ms = millis();
        if (fails < 65535) fails++;
        settings_save_ble_fail_count(fails);
        send_response("{\"ok\":false,\"error\":\"invalid_pin\"}");
        return false;
    }
    // Success: reset the persistent fail counter.
    if (fails != 0) settings_save_ble_fail_count(0);
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
    // NOTE: do NOT log the raw `json` here — it contains the BLE PIN on every
    // PIN-protected command. The command name is logged after parsing below.
    (void)json;
    if (!check_rate_limit()) { LOG_PRINTLN(F("[BLE] rate limited")); return; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { LOG_PRINTLN(F("[BLE] bad json")); send_response("{\"ok\":false,\"error\":\"bad_json\"}"); return; }

    const char* cmd = doc["cmd"] | "";
    LOG_PRINT("[BLE] cmd: %s\n", cmd);
    if (strcmp(cmd, "set_wifi") == 0) {
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
        bool enabled = doc["enabled"] | true;
        settings_save_http_endpoint(doc["url"], doc["token"]);
        settings_save_http_enabled(enabled);
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"msg\":\"http_saved\",\"enabled\":%s}",
            enabled ? "true" : "false");
        send_response(buf);
    } else if (strcmp(cmd, "set_switch") == 0 || strcmp(cmd, "set_relay") == 0) {
        if (!check_pin(doc)) return;
        uint8_t idx = doc["idx"] | 0;
        // switch_controller supports up to 8 switches (MAX_SWITCHES).
        if (idx > 7) { send_error(cmd, "invalid_idx"); return; }
        uint8_t default_switch_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
        // Resolve the GPIO pin. Only idx 0-3 have a board default; idx 4-7
        // must supply an explicit gpio_pin. Reading default_switch_pins[idx]
        // for idx >= 4 would read past the 4-entry array (stack garbage → NVS).
        int8_t pin_in;
        if (doc["gpio_pin"].is<int>()) {
            pin_in = (int8_t)doc["gpio_pin"].as<int>();
        } else if (idx < (int)(sizeof(default_switch_pins) / sizeof(default_switch_pins[0]))) {
            pin_in = default_switch_pins[idx];
        } else {
            send_error(cmd, "gpio_required");
            return;
        }
        // Validate the pin BEFORE we save to NVS — refusing a strapping /
        // USB / out-of-range pin avoids corrupting a future boot.
        if (!switch_gpio_allowed(pin_in)) {
            send_error(cmd, "gpio_reserved");
            return;
        }
        SwitchChannel ch = {};
        ch.idx = idx;
        ch.type = doc["type"] | SW_RELAY;
        ch.gpio_pin = pin_in;
        ch.active_high = doc["active_high"] | true;
        ch.enabled = doc["enabled"] | true;
        ch.is_energized = get_switch_state(idx);
        // Only set the friendly name from JSON if provided. If a follow-up
        // payload omits `name`, the existing stored name is preserved instead
        // of being clobbered back to the default "Switch N".
        if (doc["name"].is<const char*>()) {
            strlcpy(ch.name, doc["name"].as<const char*>(), sizeof(ch.name));
        } else {
            // First save: keep the default. Subsequent saves: load existing
            // name so we don't wipe a friendly name the dashboard already set.
            if (idx < settings_load_switch_count()) {
                SwitchChannel existing;
                if (settings_load_switch(idx, &existing) && existing.name[0] != '\0') {
                    strlcpy(ch.name, existing.name, sizeof(ch.name));
                } else {
                    snprintf(ch.name, sizeof(ch.name), "Switch %u", (unsigned)idx);
                }
            } else {
                snprintf(ch.name, sizeof(ch.name), "Switch %u", (unsigned)idx);
            }
        }
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
        // NOTE: this command only persists the rule and channel config. It
        // does NOT change the energised state. To force a switch ON or OFF
        // out-of-band, the caller must issue a separate `switch N 0|1` over
        // the serial CLI (or use switch_set() in code). If `switch auto on`
        // is in effect, any manual set will be overwritten by the sensor
        // task within ~1 second.
        send_response("{\"ok\":true,\"msg\":\"switch_saved\"}");
    } else if (strcmp(cmd, "set_battery") == 0) {
        if (!check_pin(doc)) return;
        // Shape discrimination: payload with `v: 2` binds a BatteryChemistryProfile
        // to a channel (new id-based path). Without `v`, treat as legacy flat
        // BatteryConfig payload (capacity_mAh/initial_soc_pct) so older dashboards
        // that haven't migrated keep working. Remove the legacy branch once the
        // dashboard is updated to always send `v: 2`.
        if (doc["v"].is<int>() && doc["v"].as<int>() == 2) {
            uint8_t ch = doc["channel"] | 0;
            if (!valid_channel(ch)) { send_error(cmd, "bad_channel"); return; }
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
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"msg\":\"battery_bound\",\"channel\":%u,\"profile_id\":%d}",
                (unsigned)ch,
                (pid == BATTERY_CHANNEL_NO_BINDING) ? -1 : (int)pid);
            send_response(buf);
        } else {
            BatteryConfig bat = {};
            bat.channel = doc["channel"] | 0;
            bat.capacity_mAh = doc["capacity_mAh"] | 0.0f;
            bat.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
            settings_save_battery(bat.channel, &bat);
            send_response("{\"ok\":true,\"msg\":\"battery_saved\"}");
        }
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
        // 0 is rejected to prevent the "open device" state where any client
        // can reconfigure the device. Allowed range: 1..999999.
        if (new_pin == 0 || new_pin > 999999) {
            send_error(cmd, "pin must be 1..999999, 0 not allowed");
            return;
        }
        settings_save_ble_pin(new_pin);
        sync_ble_pin_to_supabase();
        send_response("{\"ok\":true,\"msg\":\"pin_updated\"}");
    } else if (strcmp(cmd, "get_status") == 0) {
        if (!check_pin(doc)) return;
        DeviceState st;
        build_device_state(&st);
        JsonDocument resp;
        resp["ok"] = true;
        resp["uptime_ms"] = st.uptime_ms;
        resp["free_heap"] = st.free_heap;
        resp["min_free_heap"] = st.min_free_heap;
        resp["reset_reason"] = st.reset_reason;
        resp["wifi"] = st.wifi_connected;
        resp["rssi"] = st.wifi_rssi;
        if (st.wifi_connected) resp["ip"] = st.wifi_ip;
        resp["ntp"] = st.ntp_synced;
        resp["ble"] = st.ble_active;
        resp["ble_conn"] = st.ble_connected;
        resp["mqtt"] = st.mqtt_connected;
        resp["http"] = st.http_configured;
        resp["supabase"] = st.supabase_configured;
        resp["offline"] = st.network_skipped;
        resp["sd"] = st.sd_present;
        resp["entries"] = (uint32_t)st.log_entries;
        resp["buf_pct"] = (uint32_t)st.log_buffer_used_pct;
        resp["overflow"] = st.log_overflow;
        resp["channels"] = st.channel_count;
        resp["switches"] = st.switch_count;
        resp["calibrating"] = st.sensors_calibrating;
        char buf[512];
        serializeJson(resp, buf);
        send_response(buf);
    } else if (strcmp(cmd, "reset_coulomb") == 0) {
        if (!check_pin(doc)) return;
        uint8_t ch = doc["channel"] | 0;
        if (!valid_channel(ch)) { send_error(cmd, "bad_channel"); return; }
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
        // Partial update: only overwrite a field when the caller provided a
        // non-empty value. The old `doc["x"] | ""` fallback wrote an empty
        // string for any omitted field, clobbering existing secrets and
        // bricking Supabase connectivity on a partial BLE update.
        if (doc["url"].is<const char*>() && doc["url"].as<const char*>()[0]) {
            settings_save_supabase_url(doc["url"].as<const char*>());
        }
        if (doc["anon_key"].is<const char*>() && doc["anon_key"].as<const char*>()[0]) {
            settings_save_supabase_anon_key(doc["anon_key"].as<const char*>());
        }
        if (doc["api_key"].is<const char*>() && doc["api_key"].as<const char*>()[0]) {
            settings_save_supabase_api_key(doc["api_key"].as<const char*>());
        }
        if (doc["device_key"].is<const char*>() && doc["device_key"].as<const char*>()[0]) {
            settings_save_supabase_device_key(doc["device_key"].as<const char*>());
        }
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
        // Shape discrimination: payload with `v: 2` addresses the new
        // BatteryChemistryProfile (id-based, runtime-mutable). Without `v`,
        // treat as legacy per-channel BatteryProfile payload so older
        // dashboards keep working. Remove the legacy branch once the
        // dashboard is updated to always send `v: 2`.
        if (doc["v"].is<int>() && doc["v"].as<int>() == 2) {
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
        } else {
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
        }
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
        resp["soh_pct"] = st.soh_pct;
        resp["soh_samples"] = st.soh_samples;
        char buf[384];
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
        resp["last_session_start_pct"] = st.last_session_start_pct;
        resp["last_SoC_pct"] = st.last_SoC_pct;
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
        mark_clean_shutdown();
        ESP.restart();
    } else if (strcmp(cmd, "ota_check") == 0) {
        ota_trigger_check();
        send_ok(cmd, "ota check triggered");
    } else if (strcmp(cmd, "ota_status") == 0) {
        StaticJsonDocument<256> resp;
        resp["ok"] = true;
        resp["cmd"] = cmd;
        resp["state"] = (int)ota_get_state();
        const char* state_names[] = {"idle","checking","downloading","applying","rebooting","failed"};
        int st = (int)ota_get_state();
        if (st >= 0 && st < 6) resp["state_name"] = state_names[st];
        resp["version"] = ota_get_version();
        resp["progress"] = ota_get_progress_pct();
        resp["error"] = ota_get_last_error();
        resp["poll_interval_s"] = ota_get_poll_interval();
        char buf[512];
        serializeJson(resp, buf, sizeof(buf));
        send_response(buf);
    } else if (strcmp(cmd, "ota_set_interval") == 0) {
        uint32_t interval = doc["interval"] | 0;
        if (interval < OTA_POLL_INTERVAL_MIN_S) interval = OTA_POLL_INTERVAL_MIN_S;
        if (interval > OTA_POLL_INTERVAL_MAX_S) interval = OTA_POLL_INTERVAL_MAX_S;
        ota_set_poll_interval(interval);
        send_ok(cmd, "poll interval updated");
    } else if (strcmp(cmd, "ota_start") == 0) {
        ota_trigger_check();
        send_ok(cmd, "ota check triggered");
    } else {
        // DEBUG-level log so unrecognised commands are visible in
        // CORE_DEBUG_LEVEL=3 builds but don't spam release logs.
#if CORE_DEBUG_LEVEL >= 3
        LOG_PRINT("[BLE] unknown cmd: \"%s\"\n", cmd);
#endif
        send_error(cmd, "unknown_cmd");
    }
}

// Apply a settings command from Supabase (no PIN check — Supabase auth is trusted).
// Reuses the same command dispatch logic from handle_command, but without PIN requirement.
// cmd_type: command name string (e.g. "set_wifi", "set_calibration")
// payload_json: JSON string containing the command parameters
//
// Returns true on successful apply, false on rejection (unknown command,
// bad payload, invalid value). Callers should only run the post-hook
// (WiFi/MQTT reconnect, supabase client reset) when this returns true.
//
// TODO: Supabase auth is the trust boundary; device_api_key verification is a
// schema-side concern. The schema-fix agent will add a device_api_key column
// and validation on claim_settings_command. Until then any caller able to
// insert a row into the claimed-commands table can run these — but that is
// the pre-fix behavior, and the schema fix is the proper fix.

// Verify a PIN-bearing payload for the Supabase command channel (which has
// no BLE response to send). Destructive commands (set_pin/factory_reset/
// reboot) must carry the current PIN so an attacker who can insert a command
// row but does not know the device PIN cannot wipe, reboot, or change the
// PIN and lock out the owner.
static bool supa_pin_ok(JsonDocument& doc) {
    uint32_t expected = settings_load_ble_pin();
    if (expected == 0) return true; // no security configured
    uint32_t provided = doc["pin"].is<const char*>()
                            ? (uint32_t)atoi(doc["pin"].as<const char*>())
                            : (uint32_t)(doc["pin"] | 0);
    return provided == expected;
}

bool ble_is_active() {
    return ble_initialized;
}

bool ble_is_connected() {
    return bleClientConnected;
}

bool apply_settings_command(const char* cmd_type, const char* payload_json) {
    LOG_PRINT("[CMD] applying: %s\n", cmd_type);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload_json);
    if (err) { LOG_PRINTLN("[CMD] bad json payload"); return false; }

    if (strcmp(cmd_type, "set_wifi") == 0) {
        if (doc["ssid"] && doc["pass"]) {
            settings_save_wifi(doc["ssid"], doc["pass"]);
            LOG_PRINTLN("[CMD] wifi saved");
            return true;
        }
        return false;
    } else if (strcmp(cmd_type, "set_mqtt") == 0) {
        if (doc["broker"]) {
            // Partial-update safety: don't clobber topic with empty strings.
            const char* broker = doc["broker"] | "";
            uint16_t port     = doc["port"]     | 1883;
            if (doc["topic"].is<const char*>()) {
                const char* t = doc["topic"].as<const char*>();
                settings_save_mqtt(broker, port, t);
            } else {
                char cur_topic[64] = ""; uint16_t cur_port = 0; char cur_broker[64] = "";
                if (settings_load_mqtt(cur_broker, &cur_port, cur_topic, sizeof(cur_broker))) {
                    settings_save_mqtt(broker, port, cur_topic);
                } else {
                    settings_save_mqtt(broker, port, "");
                }
            }
            LOG_PRINTLN("[CMD] mqtt saved");
            return true;
        }
        return false;
    } else if (strcmp(cmd_type, "set_http") == 0) {
        if (doc["url"]) {
            settings_save_http_endpoint(doc["url"], doc["token"] | "");
            settings_save_http_enabled(doc["enabled"] | true);
            LOG_PRINTLN("[CMD] http saved");
            return true;
        }
        return false;
    } else if (strcmp(cmd_type, "set_supabase") == 0) {
        if (doc["url"]) {
            settings_save_supabase_url(doc["url"]);
            // Partial-update safety: a payload that omits api_key /
            // device_key / anon_key must not silently clear them. Only
            // overwrite when the field is present and non-empty.
            if (doc["anon_key"].is<const char*>() && strlen(doc["anon_key"].as<const char*>()) > 0) {
                settings_save_supabase_anon_key(doc["anon_key"]);
            }
            if (doc["api_key"].is<const char*>() && strlen(doc["api_key"].as<const char*>()) > 0) {
                settings_save_supabase_api_key(doc["api_key"]);
            }
            if (doc["device_key"].is<const char*>() && strlen(doc["device_key"].as<const char*>()) > 0) {
                settings_save_supabase_device_key(doc["device_key"]);
            }
            LOG_PRINTLN("[CMD] supabase saved");
            return true;
        }
        return false;
    } else if (strcmp(cmd_type, "set_shunt") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_shunt(ch, doc["ohms"] | 0.0f);
        LOG_PRINTLN("[CMD] shunt saved");
        return true;
    } else if (strcmp(cmd_type, "set_volt_ratio") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_volt_ratio(ch, doc["ratio"] | 0.0f);
        LOG_PRINTLN("[CMD] volt_ratio saved");
        return true;
    } else if (strcmp(cmd_type, "set_resistors") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_resistors(ch, doc["r_high"] | 0.0f, doc["r_low"] | 0.0f);
        LOG_PRINTLN("[CMD] resistors saved");
        return true;
    } else if (strcmp(cmd_type, "set_switch") == 0 || strcmp(cmd_type, "set_relay") == 0) {
        uint8_t idx = doc["idx"] | 0;
        if (idx > 7) { LOG_PRINTLN("[CMD] invalid switch idx"); return false; }
        uint8_t default_switch_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
        // Validate pin if explicit (mirrors the BLE set_switch path).
        int8_t pin_in2 = (int8_t)(doc["gpio_pin"] | default_switch_pins[idx]);
        if (doc["gpio_pin"].is<int>() && !switch_gpio_allowed(pin_in2)) {
            LOG_PRINTLN("[CMD] gpio_reserved");
            return false;
        }
        SwitchChannel ch = {};
        ch.idx = idx;
        ch.type = doc["type"] | SW_RELAY;
        ch.gpio_pin = pin_in2;
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
                else                                            c.kind = SCK_DISABLED;

                // op — accept strings (">", "<", ">=", "<=", "==") or fall
                // back to "gt". Matches the BLE path.
                const char* op = co["op"] | "gt";
                if      (strcmp(op, ">")  == 0) c.op = SCO_GT;
                else if (strcmp(op, "<")  == 0) c.op = SCO_LT;
                else if (strcmp(op, ">=") == 0) c.op = SCO_GTE;
                else if (strcmp(op, "<=") == 0) c.op = SCO_LTE;
                else if (strcmp(op, "==") == 0) c.op = SCO_EQ;
                else if (strcmp(op, "gt")  == 0) c.op = SCO_GT;
                else if (strcmp(op, "lt")  == 0) c.op = SCO_LT;
                else if (strcmp(op, "gte") == 0) c.op = SCO_GTE;
                else if (strcmp(op, "lte") == 0) c.op = SCO_LTE;
                else if (strcmp(op, "eq")  == 0) c.op = SCO_EQ;
                else                                c.op = SCO_GT;

                c.value       = co["value"] | 0.0f;
                c.ref_channel = (int8_t)(co["ref_channel"] | -1);

                // schedule_mask — copy byte-by-byte, clamped to SC_SCHEDULE_BYTES
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
            uint8_t n = 0;
            float overA = doc["overcurrent_A"]  | 0.0f;
            float undV  = doc["undervoltage_V"] | 0.0f;
            float socLo = doc["soc_low_pct"]    | 0.0f;
            // Legacy default: omit SoC conditions unless the dashboard
            // explicitly sets them. A 100% high SoC trip would trip
            // immediately on any finite SoC, which is almost certainly
            // not what the user wants.
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
        LOG_PRINTLN("[CMD] switch saved");
        return true;
    } else if (strcmp(cmd_type, "set_battery") == 0) {
        BatteryConfig bat = {};
        bat.channel = doc["channel"] | 0;
        bat.capacity_mAh = doc["capacity_mAh"] | 0.0f;
        bat.initial_soc_pct = doc["initial_soc_pct"] | 100.0f;
        settings_save_battery(bat.channel, &bat);
        LOG_PRINTLN("[CMD] battery saved");
        return true;
    } else if (strcmp(cmd_type, "set_battery_profile") == 0) {
        // Two distinct shapes hit this branch:
        //   1. settings_manager's per-channel BatteryProfile (channel-bound,
        //      has `system_voltage`, `cell_count`, `full_voltage`).
        //   2. battery_profile's chemistry registry (`id`,
        //      `nominal_voltage`, `c_rating`, `charge_efficiency`,
        //      `cycle_life_rated`, `min_soc_pct`, `max_soc_pct`) — the
        //      shape the dashboard sends.
        if (doc["id"].is<int>() || doc["id"].is<unsigned int>()) {
            uint8_t id = doc["id"] | 0;
            if (id >= BATTERY_MAX_PROFILES) {
                LOG_PRINTLN("[CMD] battery_profile id out of range");
                return false;
            }
            BatteryChemistryProfile p = {};
            const BatteryChemistryProfile* existing = battery_profile_get(id);
            if (existing) p = *existing;
            p.id = id;
            if (doc["name"].is<const char*>()) strlcpy(p.name, doc["name"] | "", sizeof(p.name));
            if (doc["chemistry"].is<int>()) p.chemistry = doc["chemistry"] | p.chemistry;
            p.nominal_voltage    = doc["nominal_voltage"]    | p.nominal_voltage;
            p.rated_capacity_Ah  = doc["rated_capacity_Ah"]  | p.rated_capacity_Ah;
            p.c_rating           = doc["c_rating"]           | p.c_rating;
            p.cutoff_voltage     = doc["cutoff_voltage"]     | p.cutoff_voltage;
            p.float_voltage      = doc["float_voltage"]      | p.float_voltage;
            p.charge_efficiency  = doc["charge_efficiency"]  | p.charge_efficiency;
            p.cycle_life_rated   = doc["cycle_life_rated"]   | p.cycle_life_rated;
            p.min_soc_pct        = doc["min_soc_pct"]        | p.min_soc_pct;
            p.max_soc_pct        = doc["max_soc_pct"]        | p.max_soc_pct;
            if (!battery_profile_set(&p)) {
                LOG_PRINTLN("[CMD] battery_profile_set failed");
                return false;
            }
            LOG_PRINTLN("[CMD] battery_profile (chemistry) saved");
            return true;
        }
        // Legacy per-channel BatteryProfile
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
        LOG_PRINTLN("[CMD] battery_profile (channel) saved");
        return true;
    } else if (strcmp(cmd_type, "delete_battery_profile") == 0) {
        uint8_t id = doc["id"] | 0;
        if (!battery_profile_delete(id)) {
            LOG_PRINTLN("[CMD] delete_battery_profile failed");
            return false;
        }
        LOG_PRINTLN("[CMD] battery_profile deleted");
        return true;
    } else if (strcmp(cmd_type, "reset_battery") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch >= MAX_LOGICAL_CHANNELS) {
            LOG_PRINTLN("[CMD] reset_battery: invalid channel");
            return false;
        }
        cycle_counter_reset(ch);
        reset_coulomb_counter(ch);
        battery_state_reset(ch);
        LOG_PRINTLN("[CMD] battery reset");
        return true;
    } else if (strcmp(cmd_type, "set_calibration") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) { LOG_PRINTLN("[CMD] invalid calibration channel"); return false; }
        uint8_t type = doc["type"] | 0;
        float value = doc["value"] | 0.0f;
        sensor_set_calibration(ch, type, value);
        LOG_PRINTLN("[CMD] calibration saved");
        return true;
    } else if (strcmp(cmd_type, "reset_calibration") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) { LOG_PRINTLN("[CMD] invalid calibration channel"); return false; }
        sensor_reset_calibration(ch);
        LOG_PRINTLN("[CMD] calibration reset");
        return true;
    } else if (strcmp(cmd_type, "set_invert_curr") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) { LOG_PRINTLN("[CMD] invalid invert_curr channel"); return false; }
        sensor_set_invert_curr(ch, doc["invert"] | false);
        LOG_PRINTLN("[CMD] invert_curr saved");
        return true;
    } else if (strcmp(cmd_type, "reset_invert_curr") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 2) { LOG_PRINTLN("[CMD] invalid invert_curr channel"); return false; }
        sensor_reset_invert_curr(ch);
        LOG_PRINTLN("[CMD] invert_curr reset");
        return true;
    } else if (strcmp(cmd_type, "reset_coulomb") == 0) {
        uint8_t ch = doc["channel"] | 0;
        reset_coulomb_counter(ch);
        LOG_PRINTLN("[CMD] coulomb_reset done");
        return true;
    } else if (strcmp(cmd_type, "set_virtual_channel") == 0) {
        uint8_t ch = doc["channel"] | 0;
        if (ch > 3) { LOG_PRINTLN("[CMD] invalid virtual_channel"); return false; }
        VirtualChannelConfig vc = {};
        vc.voltage_src = doc["voltage_src"] | 0;
        vc.voltage_idx = doc["voltage_idx"] | 0;
        vc.current_src = doc["current_src"] | 0;
        vc.current_idx = doc["current_idx"] | 0;
        settings_save_virtual_channel(ch, &vc);
        LOG_PRINTLN("[CMD] virtual_channel saved");
        return true;
    } else if (strcmp(cmd_type, "set_channel_group") == 0) {
        ChannelGroup cg = {};
        cg.group_id = doc["group_id"] | 0;
        strlcpy(cg.name, doc["name"] | "", sizeof(cg.name));
        cg.icon = doc["icon"] | 0;
        cg.channel_mask = doc["channel_mask"] | 0;
        settings_save_channel_group(cg.group_id, &cg);
        LOG_PRINTLN("[CMD] channel_group saved");
        return true;
    } else if (strcmp(cmd_type, "set_channel_name") == 0) {
        uint8_t ch = doc["channel"] | 0;
        settings_save_channel_name(ch, doc["name"] | "");
        LOG_PRINTLN("[CMD] channel_name saved");
        return true;
    } else if (strcmp(cmd_type, "set_pin") == 0) {
        // Destructive: require the current PIN (as `pin`) AND the matching
        // `old_pin` before accepting a new PIN, so an attacker who can insert
        // a command row cannot change the PIN and lock out the owner.
        uint32_t expected = settings_load_ble_pin();
        if (expected != 0 && !supa_pin_ok(doc)) {
            LOG_PRINTLN("[CMD] set_pin rejected (pin missing/wrong)");
            return false;
        }
        uint32_t old_pin = doc["old_pin"].is<const char*>()
                              ? (uint32_t)atoi(doc["old_pin"].as<const char*>())
                              : (uint32_t)(doc["old_pin"] | 0);
        if (expected != 0 && old_pin != expected) {
            LOG_PRINTLN("[CMD] set_pin rejected (old_pin mismatch)");
            return false;
        }
        // Reject 0 — that puts the device in an "open" state where any
        // client can reconfigure it. Also reject > 999999 (6-digit limit).
        uint32_t new_pin = doc["new_pin"] | 0;
        if (new_pin == 0 || new_pin > 999999) {
            LOG_PRINTLN("[CMD] set_pin rejected (must be 1..999999)");
            return false;
        }
        settings_save_ble_pin(new_pin);
        sync_ble_pin_to_supabase();
        LOG_PRINTLN("[CMD] ble_pin updated");
        return true;
    } else if (strcmp(cmd_type, "calibrate_baseline") == 0) {
        sensor_calibrate_baseline();
        sync_calibration_to_supabase();
        LOG_PRINTLN("[CMD] baseline calibration started");
        return true;
    } else if (strcmp(cmd_type, "discover_sensors") == 0) {
        discover_sensors();
        LOG_PRINTLN("[CMD] sensor discovery complete");
        return true;
    } else if (strcmp(cmd_type, "factory_reset") == 0) {
        // Destructive: require the current PIN so a stray/inserted command
        // row can't wipe the device without knowing the PIN.
        if (!supa_pin_ok(doc)) {
            LOG_PRINTLN("[CMD] factory_reset rejected (pin missing/wrong)");
            return false;
        }
        settings_factory_reset();
        LOG_PRINTLN("[CMD] factory_reset done — rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
        return true;  // unreachable
    } else if (strcmp(cmd_type, "reboot") == 0) {
        if (!supa_pin_ok(doc)) {
            LOG_PRINTLN("[CMD] reboot rejected (pin missing/wrong)");
            return false;
        }
        LOG_PRINTLN("[CMD] rebooting");
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
        return true;  // unreachable
    } else if (strcmp(cmd_type, "ota_start") == 0) {
        if (!supa_pin_ok(doc)) {
            LOG_PRINTLN("[CMD] ota_start rejected (pin missing/wrong)");
            return false;
        }
        ota_trigger_check();
        LOG_PRINTLN("[CMD] ota check triggered via ota_start");
        return true;
    } else if (strcmp(cmd_type, "ota_check") == 0) {
        ota_trigger_check();
        return true;
    } else if (strcmp(cmd_type, "ota_status") == 0) {
        // Status is reported via telemetry and MQTT; this is a no-op trigger
        return true;
    } else if (strcmp(cmd_type, "ota_set_interval") == 0) {
        uint32_t interval = doc["interval"] | 0;
        ota_set_poll_interval(interval);
        return true;
    } else {
        LOG_PRINT("[CMD] unknown: %s\n", cmd_type);
        return false;
    }
}

void init_ble_provisioner() {
    if (ble_initialized) return;
    // Queue that decouples the NimBLE host task from command execution.
    // onWrite enqueues the raw JSON; loop_ble_provisioner() (network task)
    // drains it and runs handle_command there. This keeps NVS writes, sensor
    // calibration, WiFi reconnects, etc. off the BLE host task so it can't
    // block and drop the connection.
    if (!ble_cmd_queue) {
        ble_cmd_queue = xQueueCreate(4, BLE_CMD_BUF_SIZE);
    }
    NimBLEDevice::init(BT_DEVICE_NAME);
    // Require an encrypted link for command writes. Pairing uses LE Secure
    // Connections + bonding with no MITM (Just Works, no input/output on the
    // device) — any client can pair, but the link is then encrypted so the
    // app-layer PIN is no longer sent in plaintext over an open link. The PIN
    // still gates every command; encryption removes the sniffing vector.
    // Dashboards must pair (Just Works) before issuing commands.
    NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
    NimBLEDevice::setSecurityIOCap(3);  // BLE_SM_IO_CAP_NO_IO -> Just Works
    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ProvServerCallbacks());
    pServer->advertiseOnDisconnect(true);  // NimBLE auto-restarts advertising on disconnect
    NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

    pCmdChar = pService->createCharacteristic(
        BLE_CHAR_CMD_UUID,
        NIMBLE_PROPERTY::WRITE_ENC  // require encrypted (paired) link
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
    if (!ble_initialized) return;
    // Drain queued BLE commands here (network task) so the heavy work
    // (handle_command: NVS writes, WiFi reconnect, sensor cal, response
    // notify) runs off the NimBLE host task.
    if (ble_cmd_queue) {
        char buf[BLE_CMD_BUF_SIZE];
        while (xQueueReceive(ble_cmd_queue, buf, 0) == pdTRUE) {
            handle_command(buf);
        }
    }
    if (!bleClientConnected || !pStatusChar) return;
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
