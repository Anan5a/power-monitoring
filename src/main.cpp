#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Wire.h>
#include "config.h"
#include "sensor_manager.h"
#include "display_manager.h"
#include "connectivity_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "energy_counter.h"
#include "relay_controller.h"
#include "settings_manager.h"
#include "ble_provisioner.h"
#include "serial1_manager.h"
#include "core_shared.h"

static void print_sensor_data(const SensorData& data) {
    for (int i = 0; i < 3; i++) {
        Serial.printf("CH%d: %.2fV %.3fA (cal)\n", i, data.ads1115_volts[i], data.ina3221_current[i]);
    }
    Serial.printf("Raw INA3221 volt module (0x42): ");
    for (int i = 0; i < 3; i++) {
        Serial.printf("CH%d=%.2fmV ", i, ina3221_getVoltModuleBusVoltage(i) * 1000.0f);
    }
    Serial.println();
    Serial.printf("INA226: %.2fV %.3fA %.2fW\n", data.ina226_busV, data.ina226_current, data.ina226_power);
}

static void print_status() {
    Serial.printf("MAC: %s | IP: %s | Entries: %lu/%uKB | Overflow: %d\n",
                  WiFi.macAddress().c_str(), get_local_ip_str(), log_entries_count(), log_buffer_capacity()/1024, log_has_overflow_file());
    for (int ch = 0; ch < 4; ch++) {
        float mAh = get_coulomb_mAh(ch);
        float wh = get_energy_Wh(ch);
        BatteryConfig bat;
        float soc = -1;
        if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
            soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
            if (soc < 0) soc = 0;
            if (soc > 100) soc = 100;
        }
        if (soc >= 0) {
            Serial.printf("Ch%d: %.1fmAh %.2fWh SoC:%.1f%%\n", ch, mAh, wh, soc);
        } else {
            Serial.printf("Ch%d: %.1fmAh %.2fWh\n", ch, mAh, wh);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 0 — Network Task
// Handles: MQTT loop, HTTP publish, Supabase telemetry + settings poll
// ─────────────────────────────────────────────────────────────────────────────
static void networkTask(void* param) {
    (void)param;
    Serial.println("[Network] task started on Core 0");

    // Start BLE advertising immediately so provisioning is available
    // while WiFi is still connecting (init_connectivity may block for 10s)
    start_ble_advertising();
    init_connectivity();

    SensorData data;

    for (;;) {
        loop_connectivity();
        loop_ble_provisioner();

        // Process at most 1 sensor reading per tick to avoid burst POSTs
        if (xQueueReceive(g_sensor_queue, &data, 0) == pdTRUE) {
            publish_data(data);
            publish_data_supabase(data);
        }

        // Flush log batch (RAM + LittleFS overflow) if MQTT connected
        publish_log_batch();
        // NOTE: publish_log_batch_supabase() disabled — log entries are now
        // included in the 3-second telemetry batch via publish_data_supabase().
        // Running both concurrently causes TLS contention on ESP32-C3 heap.

        // Poll Supabase for pending settings commands
        static unsigned long last_settings_check = 0;
        if (millis() - last_settings_check >= 5000) {
            last_settings_check = millis();
            check_settings_commands();
        }

        // Retry NTP sync every 60s if not yet synced (SNTP runs in background)
        static unsigned long last_ntp_retry = 0;
        if (millis() - last_ntp_retry >= 60000) {
            last_ntp_retry = millis();
            try_sync_epoch_time();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 1 — Sensor Task
// Handles: I2C reads, logging, coulomb, relay eval, OLED display
// ─────────────────────────────────────────────────────────────────────────────
static void sensorTask(void* param) {
    (void)param;
    Serial.println("[Sensor] task started on Core 1");

    SensorData data;
    TickType_t last_wake = xTaskGetTickCount();
    unsigned long last_display_update = 0;

    for (;;) {
        data = read_sensors();
        push_sensor_data(data);
        log_sample(data, millis());
        update_coulomb_counter(data, 1.0f);
        update_energy_counter(data, 1.0f);
        evaluate_relays(data);

        // OLED display update every 5s
        if (millis() - last_display_update >= 5000) {
            last_display_update = millis();
            float total_power = 0;
            for (int ch = 0; ch < 3; ch++) {
                total_power += data.ads1115_volts[ch] * data.ina3221_current[ch];
            }
            total_power += data.ina226_power;
            update_display(data, get_local_ip_str(), total_power);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial CLI helpers
// ─────────────────────────────────────────────────────────────────────────────
static void handle_serial_cli() {
    static char line[256];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if ((c == '\n' || c == '\r') && len > 0) {
            line[len] = '\0';
            len = 0;
            // Consume trailing CRLF — prevents \n from being stored as line[0]
            while (Serial.available()) {
                char next = Serial.peek();
                if (next == '\n' || next == '\r') {
                    Serial.read();
                } else {
                    break;
                }
            }

            // Commands that use the line buffer directly (for startsWith / contains checks)
            if (strncmp(line, "relay ", 6) == 0) {
                int idx, st;
                if (sscanf(line, "relay %d %d", &idx, &st) == 2) {
                    RelayRule rt;
                    if (settings_load_relay(idx, &rt)) {
                        digitalWrite(rt.gpio_pin, st ? HIGH : LOW);
                        Serial.printf("Relay %d set to %d\n", idx, st);
                    } else {
                        Serial.println("Relay not found");
                    }
                }
            } else if (strncmp(line, "reset coulomb ", 14) == 0) {
                int ch;
                if (sscanf(line, "reset coulomb %d", &ch) == 1 && ch >= 0 && ch <= 3) {
                    reset_coulomb_counter(ch);
                    Serial.printf("Coulomb counter ch%d reset\n", ch);
                }
            } else if (strncmp(line, "reset energy ", 13) == 0) {
                int ch;
                if (sscanf(line, "reset energy %d", &ch) == 1 && ch >= 0 && ch <= 3) {
                    reset_energy_counter(ch);
                    Serial.printf("Energy counter ch%d reset\n", ch);
                }
            } else if (strncmp(line, "test relay ", 11) == 0) {
                int idx;
                if (sscanf(line, "test relay %d", &idx) == 1 && idx >= 0 && idx <= 3) {
                    RelayRule rt;
                    if (settings_load_relay(idx, &rt)) {
                        Serial.printf("Testing relay %d on GPIO %d...\n", idx, rt.gpio_pin);
                        Serial.println("Activating 3s...");
                        digitalWrite(rt.gpio_pin, HIGH);
                        vTaskDelay(pdMS_TO_TICKS(3000));
                        digitalWrite(rt.gpio_pin, LOW);
                        Serial.println("Relay deactivated.");
                    } else {
                        Serial.println("Not configured. Use 'relay N 0/1' to manual override.");
                    }
                } else {
                    Serial.println("Usage: test relay 0-3");
                }
            } else if (strcmp(line, "shunt show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float s; bool ok = settings_load_shunt(ch, &s);
                    if (ok) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.6f", s);
                        Serial.printf("CH%d shunt: %s\n", ch, buf);
                    } else {
                        Serial.printf("CH%d shunt: (default)\n", ch);
                    }
                }
            } else if (strncmp(line, "shunt ", 6) == 0) {
                int ch; float ohms;
                if (sscanf(line, "shunt %d %f", &ch, &ohms) == 2 && ch >= 0 && ch <= 3) {
                    if (ohms <= 0.0f) {
                        Serial.printf("CH%d shunt cleared (using default)\n", ch);
                    } else {
                        Serial.printf("CH%d shunt set to %.6f Ohm\n", ch, ohms);
                    }
                    settings_save_shunt(ch, ohms);
                    apply_settings_posthook("set_shunt");
                } else {
                    Serial.println("Usage: shunt N ohms (e.g. shunt 0 0.0003) or shunt N 0 to clear");
                }
            } else if (strcmp(line, "vratio show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float r; bool ok = settings_load_volt_ratio(ch, &r);
                    float def = (ch == 0) ? VOLT_RATIO_CH0 : (ch == 1) ? VOLT_RATIO_CH1 : VOLT_RATIO_CH2;
                    if (ok) {
                        Serial.printf("CH%d vratio: %.4f\n", ch, r);
                    } else {
                        Serial.printf("CH%d vratio: default:%.4f\n", ch, def);
                    }
                }
            } else if (strncmp(line, "vratio ", 7) == 0) {
                int ch; float ratio;
                if (sscanf(line, "vratio %d %f", &ch, &ratio) == 2 && ch >= 0 && ch <= 2) {
                    if (ratio <= 0.0f) {
                        Serial.printf("CH%d vratio cleared\n", ch);
                    } else {
                        Serial.printf("CH%d vratio set to %.4f\n", ch, ratio);
                    }
                    settings_save_volt_ratio(ch, ratio);
                    apply_settings_posthook("set_volt_ratio");
                } else {
                    Serial.println("Usage: vratio N ratio (e.g. vratio 2 3.521) or vratio N 0 to clear");
                }
            } else if (strcmp(line, "resistor show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float rh, rl; bool ok = settings_load_resistors(ch, &rh, &rl);
                    if (ok) {
                        Serial.printf("CH%d resistors: %.0f+%.0f = %.4f\n", ch, rh, rl, (rh+rl)/rl);
                    } else {
                        Serial.printf("CH%d resistors: (not set)\n", ch);
                    }
                }
            } else if (strncmp(line, "resistor ", 9) == 0) {
                int ch; float rh, rl;
                if (sscanf(line, "resistor %d %f %f", &ch, &rh, &rl) == 3 && ch >= 0 && ch <= 2) {
                    if (rh <= 0.0f || rl <= 0.0f) {
                        Serial.printf("CH%d resistors cleared\n", ch);
                    } else {
                        float ratio = (rh + rl) / rl;
                        Serial.printf("CH%d R=%.0f+%.0f -> ratio=%.4f\n", ch, rh, rl, ratio);
                    }
                    settings_save_resistors(ch, rh, rl);
                    apply_settings_posthook("set_resistors");
                } else {
                    Serial.println("Usage: resistor N r_high r_low (e.g. resistor 2 900000 68000)");
                }
            } else if (strcmp(line, "cal show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float vo, vg, co, cg;
                    sensor_get_calibration(ch, &vo, &vg, &co, &cg);
                    Serial.printf("CH%d: vo=%.2fmV vg=%.4f co=%.2fmA cg=%.4f\n", ch, vo, vg, co, cg);
                }
            } else if (strncmp(line, "cal ", 4) == 0) {
                int ch, type; float value;
                if (sscanf(line, "cal %d %d %f", &ch, &type, &value) == 3 && ch >= 0 && ch <= 2 && type >= 0 && type <= 3) {
                    sensor_set_calibration(ch, type, value);
                    Serial.printf("CH%d cal type=%d value=%.4f saved\n", ch, type, value);
                } else {
                    Serial.println("Usage: cal N type value — type: 0=volt_offset_mv, 1=volt_gain, 2=curr_offset_ma, 3=curr_gain");
                }
            } else if (strncmp(line, "wifi_ssid ", 9) == 0) {
                char new_ssid[64];
                if (sscanf(line, "wifi_ssid %s", new_ssid) == 1) {
                    char old_ssid[64] = "", old_pass[64] = "";
                    settings_load_wifi(old_ssid, old_pass, sizeof(old_ssid));
                    settings_save_wifi(new_ssid, old_pass);
                    Serial.printf("WiFi SSID set to: %s\n", new_ssid);
                } else {
                    Serial.println("Usage: wifi_ssid <ssid>");
                }
            } else if (strncmp(line, "wifi_pass ", 10) == 0) {
                char new_pass[64];
                if (sscanf(line, "wifi_pass %s", new_pass) == 1) {
                    char old_ssid[64] = "", old_pass[64] = "";
                    settings_load_wifi(old_ssid, old_pass, sizeof(old_ssid));
                    settings_save_wifi(old_ssid, new_pass);
                    Serial.println("WiFi password updated");
                } else {
                    Serial.println("Usage: wifi_pass <pass>");
                }
            } else if (strncmp(line, "set_wifi ", 9) == 0) {
                char new_ssid[64], new_pass[64];
                if (sscanf(line, "set_wifi %s %s", new_ssid, new_pass) == 2) {
                    settings_save_wifi(new_ssid, new_pass);
                    apply_settings_posthook("set_wifi");
                    Serial.printf("WiFi set: %s (reconnecting)\n", new_ssid);
                } else {
                    Serial.println("Usage: set_wifi <ssid> <pass>");
                }
            } else if (strncmp(line, "supabase ", 9) == 0) {
                char url[128], anon_key[128], device_api_key[128], device_key[64];
                int n = sscanf(line, "supabase %s %s %s %s",
                    url, anon_key, device_api_key, device_key);
                if (n == 4) {
                    settings_save_supabase_url(url);
                    settings_save_supabase_anon_key(anon_key);
                    settings_save_supabase_api_key(device_api_key);
                    settings_save_supabase_device_key(device_key);
                    apply_settings_posthook("set_supabase");
                    Serial.println("Supabase configured (client reset).");
                } else {
                    Serial.println("Usage: supabase <url> <anon_key> <device_api_key> <device_key>");
                }
            } else if (strncmp(line, "test sensor ", 12) == 0) {
                int ch;
                if (sscanf(line, "test sensor %d", &ch) == 1 && ch >= 0 && ch <= 2) {
                    SensorData d = read_sensors();
                    Serial.printf("Sensor CH%d: %.3fV, %.3fA\n", ch, d.ads1115_volts[ch], d.ina3221_current[ch]);
                    Serial.printf("  raw shunt voltage: %.2fmV\n", ina3221_getShuntVoltage(ch) * 1000.0f);
                } else {
                    Serial.println("Usage: test sensor 0-2");
                }
            } else if (strncmp(line, "virtual_channel ", 16) == 0) {
                int ch, vs = -1, vidx = -1, cs = -1, cidx = -1;
                int n = sscanf(line, "virtual_channel %d %d %d %d %d", &ch, &vs, &vidx, &cs, &cidx);
                if (n == 1 && ch >= 0 && ch <= 3) {
                    VirtualChannelConfig vc;
                    if (settings_load_virtual_channel(ch, &vc)) {
                        Serial.printf("CH%d: V=src%d:idx%d I=src%d:idx%d\n",
                            ch, vc.voltage_src, vc.voltage_idx, vc.current_src, vc.current_idx);
                    } else {
                        Serial.printf("CH%d: not configured\n", ch);
                    }
                } else if (n == 5 && ch >= 0 && ch <= 3 && vs >= 0 && vidx >= 0 && cs >= 0 && cidx >= 0) {
                    VirtualChannelConfig vc = {};
                    vc.voltage_src = (uint8_t)vs;
                    vc.voltage_idx = (uint8_t)vidx;
                    vc.current_src = (uint8_t)cs;
                    vc.current_idx = (uint8_t)cidx;
                    settings_save_virtual_channel(ch, &vc);
                    Serial.printf("CH%d: V=src%d:idx%d I=src%d:idx%d saved\n", ch, vs, vidx, cs, cidx);
                } else {
                    Serial.println("Usage: virtual_channel show | virtual_channel N | virtual_channel N vs vidx cs cidx");
                }
            } else if (strncmp(line, "display ", 8) == 0) {
                SensorData d = read_sensors();
                float total_power = 0;
                for (int ch = 0; ch < 3; ch++) total_power += d.ads1115_volts[ch] * d.ina3221_current[ch];
                total_power += d.ina226_power;
                if (strcmp(line, "display all") == 0) {
                    for (int p = 0; p < 5; p++) {
                        Serial.printf("=== Display page %d ===\n", p);
                        if (p == 0) {
                            Serial.printf("  IP: %s | Power: %.1fW | Log: %lu %s\n",
                                get_local_ip_str(), total_power, log_entries_count(),
                                log_has_overflow_file() ? "[OVF]" : "");
                        } else {
                            uint8_t ch = p - 1;
                            char name[24] = "";
                            settings_load_channel_name(ch, name, sizeof(name));
                            if (!name[0]) {
                                if (ch == 0) strlcpy(name, "Battery", sizeof(name));
                                else if (ch == 1) strlcpy(name, "Solar", sizeof(name));
                                else strlcpy(name, "Output", sizeof(name));
                            }
                            float v = d.ads1115_volts[ch];
                            float i = d.ina3221_current[ch];
                            float pw = v * i;
                            Serial.printf("  Ch%d (%s): %.2fV %.3fA %.2fW\n", ch, name, v, i, pw);
                            float mAh = get_coulomb_mAh(ch);
                            BatteryConfig bat;
                            float soc = -1;
                            if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
                                soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
                                if (soc < 0) soc = 0;
                                if (soc > 100) soc = 100;
                                Serial.printf("  SoC: %.0f%% | mAh: %.0f\n", soc, mAh);
                            } else {
                                Serial.printf("  mAh: %.0f\n", mAh);
                            }
                        }
                    }
                    Serial.printf("INA226: %.2fV %.3fA %.2fW\n", d.ina226_busV, d.ina226_current, d.ina226_power);
                } else {
                    int pn;
                    if (sscanf(line, "display page %d", &pn) == 1 && pn >= 0 && pn <= 4) {
                        Serial.printf("=== Display page %d ===\n", pn);
                        if (pn == 0) {
                            Serial.printf("  IP: %s | Power: %.1fW | Log: %lu %s\n",
                                get_local_ip_str(), total_power, log_entries_count(),
                                log_has_overflow_file() ? "[OVF]" : "");
                        } else {
                            uint8_t ch = pn - 1;
                            char name[24] = "";
                            settings_load_channel_name(ch, name, sizeof(name));
                            if (!name[0]) {
                                if (ch == 0) strlcpy(name, "Battery", sizeof(name));
                                else if (ch == 1) strlcpy(name, "Solar", sizeof(name));
                                else strlcpy(name, "Output", sizeof(name));
                            }
                            float v = d.ads1115_volts[ch];
                            float i = d.ina3221_current[ch];
                            float pw = v * i;
                            Serial.printf("  Ch%d (%s): %.2fV %.3fA %.2fW\n", ch, name, v, i, pw);
                            float mAh = get_coulomb_mAh(ch);
                            BatteryConfig bat;
                            float soc = -1;
                            if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
                                soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
                                if (soc < 0) soc = 0;
                                if (soc > 100) soc = 100;
                                Serial.printf("  SoC: %.0f%% | mAh: %.0f\n", soc, mAh);
                            } else {
                                Serial.printf("  mAh: %.0f\n", mAh);
                            }
                        }
                    } else {
                        Serial.println("Usage: display page 0-4  |  display all");
                    }
                }
            // Exact-match commands below (no startsWith variants)
            } else if (strcmp(line, "status") == 0) {
                print_status();
            } else if (strcmp(line, "mem") == 0) {
                Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
                Serial.printf("Heap size: %u bytes\n", ESP.getHeapSize());
                Serial.printf("Min free heap: %u bytes\n", ESP.getMinFreeHeap());
                Serial.printf("CPU temperature: %.1f C\n", temperatureRead());
                Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());
            } else if (strcmp(line, "sensors") == 0) {
                SensorData data = read_sensors();
                print_sensor_data(data);
            } else if (strcmp(line, "relay status") == 0) {
                uint8_t count = settings_load_relay_count();
                for (uint8_t i = 0; i < count; i++) {
                    RelayRule rt;
                    if (settings_load_relay(i, &rt)) {
                        int state = digitalRead(rt.gpio_pin);
                        Serial.printf("Relay %d: ch=%d pin=%d state=%d\n", i, rt.channel, rt.gpio_pin, state);
                    }
                }
            } else if (strcmp(line, "flush log") == 0) {
                size_t flushed = 0;
                uint8_t batch[512];
                size_t n;
                while ((n = log_pop_batch(batch, sizeof(batch))) > 0) {
                    flushed += n;
                }
                Serial.printf("Flushed %u bytes from log buffer\n", (unsigned)flushed);
            } else if (strcmp(line, "i2c_scan") == 0) {
                Wire.begin(I2C_SDA, I2C_SCL);
                Serial.println("I2C scan:");
                for (uint8_t addr = 1; addr < 127; addr++) {
                    Wire.beginTransmission(addr);
                    if (Wire.endTransmission() == 0) {
                        Serial.print("  0x"); Serial.println(addr, HEX);
                    }
                }
                Serial.println("done");
            } else if (strcmp(line, "test all relays") == 0) {
                Serial.println("Testing all relays in sequence...");
                for (int i = 0; i < 4; i++) {
                    RelayRule rt;
                    if (settings_load_relay(i, &rt)) {
                        Serial.printf("Relay %d (GPIO %d)...\n", i, rt.gpio_pin);
                        digitalWrite(rt.gpio_pin, HIGH);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        digitalWrite(rt.gpio_pin, LOW);
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
                Serial.println("All relays tested.");
            } else if (strcmp(line, "test all sensors") == 0) {
                SensorData d = read_sensors();
                Serial.println("All sensor channels:");
                for (int i = 0; i < 3; i++) {
                    Serial.printf("  CH%d: %.3fV  %.3fA\n", i, d.ads1115_volts[i], d.ina3221_current[i]);
                }
            } else if (strcmp(line, "test display") == 0) {
                Serial.println("Display test: OLED should show cycling pages");
                extern void init_display();
                extern void update_display(const SensorData&, const char*, float);
                SensorData d = read_sensors();
                for (int i = 0; i < 5; i++) {
                    update_display(d, "192.168.1.1", 123.4f);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                Serial.println("Display test done.");
            } else if (strcmp(line, "relay auto on") == 0) {
                relay_set_auto(true);
                Serial.println("Relay auto-trip ENABLED");
            } else if (strcmp(line, "relay auto off") == 0) {
                relay_set_auto(false);
                Serial.println("Relay auto-trip DISABLED");
            } else if (strcmp(line, "factory_reset") == 0) {
                Serial.println("Wiping NVS and rebooting...");
                vTaskDelay(pdMS_TO_TICKS(500));
                settings_factory_reset();
                ESP.restart();
            } else if (strcmp(line, "calibrate_baseline") == 0) {
                sensor_calibrate_baseline();
                Serial.println("Baseline recalibration started — collecting new baseline over next 10 ticks");
            } else if (strcmp(line, "ble_on") == 0) {
                start_ble_advertising();
                Serial.println("BLE advertising restarted");
            } else if (strcmp(line, "wifi_show") == 0) {
                char ssid[64] = "", pass[64] = "";
                if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
                    Serial.printf("WiFi SSID: %s\n", ssid);
                } else {
                    Serial.println("WiFi: not configured");
                }
            } else if (strcmp(line, "supabase_show") == 0) {
                char url[128] = "", anon_key[128] = "", device_key[64] = "", api_key[64] = "";
                bool has_url = settings_load_supabase_url(url, sizeof(url));
                bool has_akey = settings_load_supabase_anon_key(anon_key, sizeof(anon_key));
                bool has_dkey = settings_load_supabase_device_key(device_key, sizeof(device_key));
                bool has_apikey = settings_load_supabase_api_key(api_key, sizeof(api_key));
                if (has_url) Serial.printf("Supabase URL: %s\n", url);
                else Serial.println("Supabase URL: not set");
                if (has_akey) Serial.printf("Supabase anon key: %s\n", anon_key);
                if (has_dkey) Serial.printf("Device key: %s\n", device_key);
                if (has_apikey) Serial.printf("Device API key: %s\n", api_key);
                if (!has_url && !has_akey) Serial.println("Supabase: not configured");
            } else if (strcmp(line, "virtual_channel show") == 0) {
                Serial.println("Virtual channel configs:");
                for (int ch = 0; ch < 4; ch++) {
                    VirtualChannelConfig vc;
                    if (settings_load_virtual_channel(ch, &vc)) {
                        Serial.printf("  CH%d: V=src%d:idx%d I=src%d:idx%d\n",
                            ch, vc.voltage_src, vc.voltage_idx, vc.current_src, vc.current_idx);
                    } else {
                        Serial.printf("  CH%d: not configured\n", ch);
                    }
                }
            } else if (strcmp(line, "reboot") == 0) {
                Serial.println("Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(100));
                ESP.restart();
            } else if (strcmp(line, "serial1peek") == 0) {
#if ENABLE_SERIAL1
                char buf[80];
                int count = 0;
                while (serial1_available() > 0 && count < 5) {
                    if (serial1_read_line(buf, sizeof(buf))) {
                        Serial.printf("[S1] %s\n", buf);
                        count++;
                    }
                }
                if (count == 0) Serial.println("S1: no data");
#else
                Serial.println("Serial1 disabled");
#endif
            } else if (strcmp(line, "help") == 0) {
                Serial.println("Commands:");
                Serial.println("  status              — IP, log entries, coulomb/energy/SoC");
                Serial.println("  mem                 — free heap bytes, CPU temperature");
                Serial.println("  sensors             — all sensor readings");
                Serial.println("  relay status        — relay GPIO states");
                Serial.println("  relay N 0/1         — manual override relay N (0=off, 1=on)");
                Serial.println("  test relay N        — pulse relay N for 3s (auto-reset)");
                Serial.println("  test all relays     — pulse each relay 1s in sequence");
                Serial.println("  test sensor N       — read sensor CH N once (0-2)");
                Serial.println("  test all sensors    — read all sensor channels");
                Serial.println("  test display        — cycle OLED pages 5x");
                Serial.println("  display page N     — print display page N (0-4)");
                Serial.println("  display all         — print all display pages");
                Serial.println("  relay auto on       — enable auto-trip (off by default)");
                Serial.println("  relay auto off      — disable auto-trip");
                Serial.println("  factory_reset       — wipe all NVS settings, reboot");
                Serial.println("  reset coulomb N     — reset coulomb counter CH N");
                Serial.println("  reset energy N      — reset energy counter CH N");
                Serial.println("  flush log           — flush RAM log buffer");
                Serial.println("  i2c_scan            — scan I2C bus for devices");
                Serial.println("  shunt N ohms        — set shunt resistance for CH N (0 clears)");
                Serial.println("  shunt show          — show current shunt settings");
                Serial.println("  vratio N ratio      — set voltage divider ratio for CH N (0 clears)");
                Serial.println("  vratio show         — show current voltage ratios");
                Serial.println("  resistor N r_high r_low — set R values, ratio auto-computed");
                Serial.println("  resistor show        — show resistor values per channel");
                Serial.println("  cal N type value    — set calibration (type: 0=vo_mv, 1=vg, 2=co_ma, 3=cg)");
                Serial.println("  cal show            — show all channel calibration values");
                Serial.println("  calibrate_baseline  — restart baseline noise calibration (10 ticks)");
                Serial.println("  ble_on              — restart BLE advertising (for re-provisioning)");
                Serial.println("  serial1peek         — dump up to 5 lines from Serial1");
                Serial.println("  wifi_show          — show current WiFi SSID");
                Serial.println("  wifi_ssid <ssid>  — set WiFi SSID");
                Serial.println("  wifi_pass <pass>  — set WiFi password");
                Serial.println("  set_wifi <ssid> <pass> — set both SSID and password");
                Serial.println("  supabase_show     — show Supabase config");
                Serial.println("  supabase <url> <anon_key> <device_api_key> <device_key>");
                Serial.println("     device_api_key = UUID from Supabase devices table (not sb_secret_...)");
                Serial.println("  reboot              — reboot the device");
                Serial.println("  help                — this list");
            } else if (len > 0) {
                Serial.println("Unknown command. Type 'help'.");
            }
        } else {
            if (len < 255) line[len++] = c;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino setup — create FreeRTOS tasks
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.setTxBufferSize(4096);
    while (!Serial) { ; }
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Power Monitor v2 starting...");

    init_settings();
    init_sensors();
    init_display();
    log_set_epoch(get_epoch_time());
    init_data_logger();
    init_coulomb_counter();
    init_energy_counter();
    init_relays();
    init_core_shared();

    Serial.printf("Free heap before BLE: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Min free heap: %u bytes\n", ESP.getMinFreeHeap());
    Serial.printf("Largest alloc: %u bytes\n", ESP.getMaxAllocHeap());
    init_ble_provisioner();  // BLE stack needs ~60-80KB contiguous heap

    xTaskCreatePinnedToCore(networkTask, "Network", 12288, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(sensorTask,    "Sensor",   4096, NULL, 4, NULL, 0);

    Serial.println("Type 'help' for serial commands");
}

void loop() {
    // Not used — all work is in FreeRTOS tasks
    static uint32_t last_heap_check = 0;
    if (millis() - last_heap_check >= 5000) {
        last_heap_check = millis();
        Serial.printf("[MEM] free=%u min=%u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    }
    handle_serial_cli();
    vTaskDelay(pdMS_TO_TICKS(10));
}