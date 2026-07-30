#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "sensor_manager.h"
#include "display_manager.h"
#include "connectivity_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "energy_counter.h"
#include "switch_controller.h"
#include "ui_manager.h"
#include "settings_manager.h"
#include "ble_provisioner.h"
#include "serial1_manager.h"
#include "core_shared.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "cycle_counter.h"
#include "log_serial.h"
#include "telemetry.h"
#include "device_state.h"
#include "device_identity.h"
#include "event_log.h"
#include "ota_client.h"

// ─────────────────────────────────────────────────────────────────────────────
// Watchdog timer (TWDT) — one-time init with the longest possible panic
// timeout, then add the three long-running tasks. Each task must call
// esp_task_wdt_reset() inside its loop or the device will panic and reboot.
// We deliberately do NOT subscribe the Arduino loop() to the WDT — that
// task is the lowest priority, has no real work, and panicking from the
// idle/serial path would mask the actual stall. Tasks added:
//   - Network: WiFi/MQTT/Supabase — any of these can stall for tens of seconds
//   - Sensor:  I2C bus reads can wedge if a slave holds the clock low
//   - UI:      debounce/display page cycle, very fast — adds a safety net
// The 30 s timeout is generous: the slowest leg (NTP sync) is bounded to 5 s
// and the next code-paths are all 10 ms-tick yielders.
// ─────────────────────────────────────────────────────────────────────────────
static void wdt_init() {
    // 30s matches what most IDF examples use and is short enough to detect
    // a wedged bus without disrupting normal MQTT/NTP work.
    esp_task_wdt_init(30, true);
}

static void print_sensor_data(const SensorSnapshot& data) {
    (void)data;
    for (int i = 0; i < 3; i++) {
        LOG_PRINT("CH%d: %.2fV %.3fA (cal)\n", i, get_channel_voltage(i), get_channel_current(i));
    }
    LOG_PRINT("Raw INA3221 volt module (0x42): ");
    for (int i = 0; i < 3; i++) {
        LOG_PRINT("CH%d=%.2fmV ", i, ina3221_getVoltModuleBusVoltage(i) * 1000.0f);
    }
    LOG_PRINTLN();
    LOG_PRINT("INA226: %.2fV %.3fA %.2fW\n", get_channel_voltage(3), get_channel_current(3), get_channel_power(3));
}

static void print_status() {
    TelemetrySnapshot snap;
    telemetry_build(snap);
    LOG_PRINT("── System ──────────────────────────────────────\n");
    LOG_PRINT("Uptime: %lu s  Heap: %u/%u min  Reset: %u  Crashes: %u%s\n",
        snap.device.uptime_ms / 1000, snap.heap_free, snap.min_free_heap,
        snap.reset_reason, snap.crash_count, snap.safe_mode ? " SAFE MODE" : "");
    LOG_PRINT("Device: %s rev %s fw %s\n", snap.device.id, snap.hw_rev, snap.device.fw);
    LOG_PRINT("── WiFi ────────────────────────────────────────\n");
    LOG_PRINT("Connected: %d  RSSI: %d dBm  IP: %s  NTP: %d\n",
        snap.wifi.rssi != 0, snap.wifi.rssi,
        snap.wifi.rssi != 0 ? snap.wifi.ip : "-", snap.ntp_synced);
    LOG_PRINT("── BLE ─────────────────────────────────────────\n");
    LOG_PRINT("Active: %d  Connected: %d\n", snap.ble_active, snap.ble_connected);
    LOG_PRINT("── Services ────────────────────────────────────\n");
    LOG_PRINT("MQTT: %d  HTTP: %d  Supabase: %d  Offline: %d\n",
        snap.mqtt_connected, snap.http_configured, snap.supabase_configured, snap.network_skipped);
    LOG_PRINT("── Storage ─────────────────────────────────────\n");
    LOG_PRINT("SD: %d  Log: %u entries (%u%%)  Overflow: %d\n",
        snap.sd_present, (unsigned)snap.log.entries,
        (unsigned)snap.log_buffer_used_pct, snap.log.overflow);
    LOG_PRINT("── Sensors ────────────────────────────────────\n");
    LOG_PRINT("Channels: %u  Switches: %u  Calibrating: %d\n",
        snap.channel_count, snap.switch_count, snap.sensors_calibrating);
    for (int ch = 0; ch < snap.channel_count && ch < 4; ch++) {
        const TelemetryChannel& tc = snap.channels[ch];
        LOG_PRINT("Ch%d: %.3fV %.3fA %.2fW  mAh:%.0f  Wh:%.2f\n",
            ch, tc.V, tc.I, tc.P, tc.charge_mAh, tc.energy_Wh);
    }
    LOG_PRINT("── Batteries ───────────────────────────────────\n");
    for (int i = 0; i < snap.battery_count; i++) {
        const TelemetryBattery& tb = snap.battery[i];
        LOG_PRINT("Ch%d: SoC=%.1f%%  SoH=%.1f%%  cycles=%.2f  Ah_in=%.2f  Ah_out=%.2f\n",
            tb.ch, tb.soc_pct, tb.soh_pct, tb.equivalent_full_cycles,
            tb.cumulative_Ah_in, tb.cumulative_Ah_out);
    }
    LOG_PRINT("── OTA ────────────────────────────────────────\n");
    LOG_PRINT("State: %s  Version: %s  Progress: %u%%  Error: %s\n",
        snap.ota.ota_status, snap.ota.ota_version,
        snap.ota.ota_progress_pct, snap.ota.ota_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 0 — Network Task
// Handles: MQTT loop, HTTP publish, Supabase telemetry + settings poll + OLED display
// ─────────────────────────────────────────────────────────────────────────────
static void networkTask(void* param) {
    (void)param;
    LOG_PRINTLN("[Network] task started on Core 0");

    // Start BLE advertising immediately so provisioning is available
    // while WiFi is still associating in the background state machine.
    start_ble_advertising();
    init_connectivity();

    // Subscribe to the TWDT. Must be called from inside the task so the
    // WDT tracks THIS task handle, not the calling task.
    esp_task_wdt_add(NULL);

    SensorSnapshot data;

    for (;;) {
        esp_task_wdt_reset();
        loop_connectivity();
        loop_ble_provisioner();
        loop_ota_client();

        // Feed UI with network/cloud status
        static bool last_wifi = false, last_cloud = false;
        bool wifi_now = (WiFi.status() == WL_CONNECTED);
        bool cloud_now = wifi_now && is_cloud_connected(); // best-effort cloud indicator
        if (wifi_now != last_wifi || cloud_now != last_cloud) {
            last_wifi = wifi_now;
            last_cloud = cloud_now;
            ui_set_network_status(wifi_now, cloud_now);
        }

        // Process at most 1 sensor reading per tick to avoid burst POSTs
        static unsigned long last_display_update = 0;
        if (xQueueReceive(g_sensor_queue, &data, 0) == pdTRUE) {
            // Build the telemetry snapshot ONCE per cycle and share it with
            // both transports. telemetry_build() clears the one-shot
            // capacity-test SoH flag, so building twice (the old pattern)
            // meant the second publish never carried capacity_test_soh_valid.
            TelemetrySnapshot snap;
            telemetry_build(snap);
            publish_data(data, snap);
            publish_data_supabase(data, snap);

            // Update OLED from network task so I2C display traffic doesn't delay
            // the 1-second sensor sampling loop.
            if (millis() - last_display_update >= 5000) {
                last_display_update = millis();
                float total_power = 0;
                for (int ch = 0; ch < 4 && ch < (int)data.total_logical_channels; ch++) {
                    total_power += get_channel_power(data, ch);
                }
                update_display(data, get_local_ip_str(), total_power);
            }
        }

        // Flush log batch to Supabase (RAM + LittleFS overflow)
        publish_log_batch_supabase();

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
// Core 1 — UI Task
// Handles: button debounce, LED patterns, display page cycling
// ─────────────────────────────────────────────────────────────────────────────
static void uiTask(void* param) {
    (void)param;
    LOG_PRINTLN("[UI] task started on Core 1");
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        loop_ui();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Core 1 — Sensor Task
// Handles: I2C reads, logging, coulomb, switch/relay eval
// ─────────────────────────────────────────────────────────────────────────────
static void sensorTask(void* param) {
    (void)param;
    LOG_PRINTLN("[Sensor] task started on Core 1");
    esp_task_wdt_add(NULL);

    SensorSnapshot data;
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_sample = last_wake;  // for real-dt integration (see below)

    for (;;) {
        esp_task_wdt_reset();
        // Measure the actual elapsed time since the last sample so the
        // integrators (coulomb/energy/cycle/capacity) use real dt, not a
        // hardcoded 1.0 s. If the task slips (slow I2C, WDT reset), integrating
        // as if exactly 1 s caused systematic energy/mAh drift. A separate
        // `last_sample` is used so vTaskDelayUntil's precise 1 s period is
        // preserved.
        TickType_t now_ticks = xTaskGetTickCount();
        float dt_seconds = (float)(now_ticks - last_sample) / (float)configTICK_RATE_HZ;
        last_sample = now_ticks;
        if (dt_seconds <= 0.0f || dt_seconds > 10.0f) dt_seconds = 1.0f; // sanity clamp

        data = read_sensors();
        push_sensor_data(data);
        log_sample(data, millis());
        update_coulomb_counter(data, dt_seconds);
        update_energy_counter(data, dt_seconds);
        update_cycle_counter(data, dt_seconds);
        evaluate_switches(data);

        // Confirm OTA firmware validity after first successful sensor cycle
        static bool ota_confirmed = false;
        if (!ota_confirmed) {
            ota_confirm_valid();
            ota_confirmed = true;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial CLI helpers
// ─────────────────────────────────────────────────────────────────────────────
static void handle_serial_cli() {
#if HAS_SERIAL
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
            if (strncmp(line, "switch ", 7) == 0 || strncmp(line, "relay ", 6) == 0) {
                int idx, st;
                char cmd[8];
                if (sscanf(line, "%7s %d %d", cmd, &idx, &st) == 3) {
                    if (idx >= 0 && idx < 8) {
                        switch_set((uint8_t)idx, st ? true : false);
                        LOG_PRINT("Switch %d set to %d\n", idx, st);
                    } else {
                        LOG_PRINTLN("Switch index out of range (0-7)");
                    }
                }
            } else if (strncmp(line, "reset coulomb ", 14) == 0) {
                int ch;
                if (sscanf(line, "reset coulomb %d", &ch) == 1 && ch >= 0 && ch <= 3) {
                    reset_coulomb_counter(ch);
                    LOG_PRINT("Coulomb counter ch%d reset\n", ch);
                }
            } else if (strncmp(line, "reset energy ", 13) == 0) {
                int ch;
                if (sscanf(line, "reset energy %d", &ch) == 1 && ch >= 0 && ch <= 3) {
                    reset_energy_counter(ch);
                    LOG_PRINT("Energy counter ch%d reset\n", ch);
                }
            } else if (strncmp(line, "test switch ", 12) == 0 || strncmp(line, "test relay ", 11) == 0) {
                int idx;
                char cmd[8];
                if ((sscanf(line, "test %7s %d", cmd, &idx) == 2) && idx >= 0 && idx < 8) {
                    LOG_PRINT("Testing switch %d for 3s...\n", idx);
                    switch_pulse((uint8_t)idx, 3000);
                } else {
                    LOG_PRINTLN("Usage: test switch/relay 0-7");
                }
            } else if (strcmp(line, "shunt show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float s; bool ok = settings_load_shunt(ch, &s);
                    if (ok) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.6f", s);
                        LOG_PRINT("CH%d shunt: %s\n", ch, buf);
                    } else {
                        LOG_PRINT("CH%d shunt: (default)\n", ch);
                    }
                }
            } else if (strncmp(line, "shunt ", 6) == 0) {
                int ch; float ohms;
                if (sscanf(line, "shunt %d %f", &ch, &ohms) == 2 && ch >= 0 && ch <= 3) {
                    if (ohms <= 0.0f) {
                        LOG_PRINT("CH%d shunt cleared (using default)\n", ch);
                    } else {
                        LOG_PRINT("CH%d shunt set to %.6f Ohm\n", ch, ohms);
                    }
                    settings_save_shunt(ch, ohms);
                    apply_settings_posthook("set_shunt");
                } else {
                    LOG_PRINTLN("Usage: shunt N ohms (e.g. shunt 0 0.0003) or shunt N 0 to clear");
                }
            } else if (strcmp(line, "vratio show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float r; bool ok = settings_load_volt_ratio(ch, &r);
                    float def = (ch == 0) ? VOLT_RATIO_CH0 : (ch == 1) ? VOLT_RATIO_CH1 : VOLT_RATIO_CH2;
                    if (ok) {
                        LOG_PRINT("CH%d vratio: %.4f\n", ch, r);
                    } else {
                        LOG_PRINT("CH%d vratio: default:%.4f\n", ch, def);
                    }
                }
            } else if (strncmp(line, "vratio ", 7) == 0) {
                int ch; float ratio;
                if (sscanf(line, "vratio %d %f", &ch, &ratio) == 2 && ch >= 0 && ch <= 2) {
                    if (ratio <= 0.0f) {
                        LOG_PRINT("CH%d vratio cleared\n", ch);
                    } else {
                        LOG_PRINT("CH%d vratio set to %.4f\n", ch, ratio);
                    }
                    settings_save_volt_ratio(ch, ratio);
                    apply_settings_posthook("set_volt_ratio");
                } else {
                    LOG_PRINTLN("Usage: vratio N ratio (e.g. vratio 2 3.521) or vratio N 0 to clear");
                }
            } else if (strcmp(line, "resistor show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float rh, rl; bool ok = settings_load_resistors(ch, &rh, &rl);
                    if (ok) {
                        LOG_PRINT("CH%d resistors: %.0f+%.0f = %.4f\n", ch, rh, rl, (rh+rl)/rl);
                    } else {
                        LOG_PRINT("CH%d resistors: (not set)\n", ch);
                    }
                }
            } else if (strncmp(line, "resistor ", 9) == 0) {
                int ch; float rh, rl;
                if (sscanf(line, "resistor %d %f %f", &ch, &rh, &rl) == 3 && ch >= 0 && ch <= 2) {
                    if (rh <= 0.0f || rl <= 0.0f) {
                        LOG_PRINT("CH%d resistors cleared\n", ch);
                    } else {
                        float ratio = (rh + rl) / rl;
                        LOG_PRINT("CH%d R=%.0f+%.0f -> ratio=%.4f\n", ch, rh, rl, ratio);
                    }
                    settings_save_resistors(ch, rh, rl);
                    apply_settings_posthook("set_resistors");
                } else {
                    LOG_PRINTLN("Usage: resistor N r_high r_low (e.g. resistor 2 900000 68000)");
                }
            } else if (strcmp(line, "cal show") == 0) {
                for (int ch = 0; ch < 3; ch++) {
                    float vo, vg, co, cg;
                    sensor_get_calibration(ch, &vo, &vg, &co, &cg);
                    LOG_PRINT("CH%d: vo=%.2fmV vg=%.4f co=%.2fmA cg=%.4f\n", ch, vo, vg, co, cg);
                }
            } else if (strncmp(line, "cal ", 4) == 0) {
                int ch, type; float value;
                if (sscanf(line, "cal %d %d %f", &ch, &type, &value) == 3 && ch >= 0 && ch <= 2 && type >= 0 && type <= 3) {
                    sensor_set_calibration(ch, type, value);
                    LOG_PRINT("CH%d cal type=%d value=%.4f saved\n", ch, type, value);
                } else {
                    LOG_PRINTLN("Usage: cal N type value — type: 0=volt_offset_mv, 1=volt_gain, 2=curr_offset_ma, 3=curr_gain");
                }
            } else if (strncmp(line, "wifi_ssid ", 9) == 0) {
                char new_ssid[64];
                if (sscanf(line, "wifi_ssid %s", new_ssid) == 1) {
                    char old_ssid[64] = "", old_pass[64] = "";
                    settings_load_wifi(old_ssid, old_pass, sizeof(old_ssid));
                    settings_save_wifi(new_ssid, old_pass);
                    LOG_PRINT("WiFi SSID set to: %s\n", new_ssid);
                } else {
                    LOG_PRINTLN("Usage: wifi_ssid <ssid>");
                }
            } else if (strncmp(line, "wifi_pass ", 10) == 0) {
                char new_pass[64];
                if (sscanf(line, "wifi_pass %s", new_pass) == 1) {
                    char old_ssid[64] = "", old_pass[64] = "";
                    settings_load_wifi(old_ssid, old_pass, sizeof(old_ssid));
                    settings_save_wifi(old_ssid, new_pass);
                    LOG_PRINTLN("WiFi password updated");
                } else {
                    LOG_PRINTLN("Usage: wifi_pass <pass>");
                }
            } else if (strncmp(line, "set_wifi ", 9) == 0) {
                char new_ssid[64], new_pass[64];
                if (sscanf(line, "set_wifi %s %s", new_ssid, new_pass) == 2) {
                    settings_save_wifi(new_ssid, new_pass);
                    apply_settings_posthook("set_wifi");
                    LOG_PRINT("WiFi set: %s (reconnecting)\n", new_ssid);
                } else {
                    LOG_PRINTLN("Usage: set_wifi <ssid> <pass>");
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
                    LOG_PRINTLN("Supabase configured (client reset).");
                } else {
                    LOG_PRINTLN("Usage: supabase <url> <anon_key> <device_api_key> <device_key>");
                }
            } else if (strncmp(line, "test sensor ", 12) == 0) {
                int ch;
                if (sscanf(line, "test sensor %d", &ch) == 1 && ch >= 0 && ch <= 2) {
                    SensorSnapshot d = read_sensors();
                    LOG_PRINT("Sensor CH%d: %.3fV, %.3fA\n", ch, get_channel_voltage(ch), get_channel_current(ch));
                    LOG_PRINT("  raw shunt voltage: %.2fmV\n", ina3221_getShuntVoltage(ch) * 1000.0f);
                } else {
                    LOG_PRINTLN("Usage: test sensor 0-2");
                }
            } else if (strncmp(line, "virtual_channel ", 16) == 0) {
                int ch, vs = -1, vidx = -1, cs = -1, cidx = -1;
                int n = sscanf(line, "virtual_channel %d %d %d %d %d", &ch, &vs, &vidx, &cs, &cidx);
                if (n == 1 && ch >= 0 && ch <= 3) {
                    VirtualChannelConfig vc;
                    if (settings_load_virtual_channel(ch, &vc)) {
                        LOG_PRINT("CH%d: V=src%d:idx%d I=src%d:idx%d\n",
                            ch, vc.voltage_src, vc.voltage_idx, vc.current_src, vc.current_idx);
                    } else {
                        LOG_PRINT("CH%d: not configured\n", ch);
                    }
                } else if (n == 5 && ch >= 0 && ch <= 3 && vs >= 0 && vidx >= 0 && cs >= 0 && cidx >= 0) {
                    VirtualChannelConfig vc = {};
                    vc.voltage_src = (uint8_t)vs;
                    vc.voltage_idx = (uint8_t)vidx;
                    vc.current_src = (uint8_t)cs;
                    vc.current_idx = (uint8_t)cidx;
                    settings_save_virtual_channel(ch, &vc);
                    LOG_PRINT("CH%d: V=src%d:idx%d I=src%d:idx%d saved\n", ch, vs, vidx, cs, cidx);
                } else {
                    LOG_PRINTLN("Usage: virtual_channel show | virtual_channel N | virtual_channel N vs vidx cs cidx");
                }
            } else if (strncmp(line, "display ", 8) == 0) {
                SensorSnapshot d = read_sensors();
                float total_power = 0;
                for (int ch = 0; ch < 4; ch++) total_power += get_channel_power(ch);
                if (strcmp(line, "display all") == 0) {
                    for (int p = 0; p < 5; p++) {
                        LOG_PRINT("=== Display page %d ===\n", p);
                        if (p == 0) {
                            LOG_PRINT("  IP: %s | Power: %.1fW | Log: %lu %s\n",
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
                            float v = get_channel_voltage(ch);
                            float i = get_channel_current(ch);
                            float pw = v * i;
                            LOG_PRINT("  Ch%d (%s): %.2fV %.3fA %.2fW\n", ch, name, v, i, pw);
                            float mAh = get_coulomb_mAh(ch);
                            BatteryConfig bat;
                            float soc = -1;
                            if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
                                soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
                                if (soc < 0) soc = 0;
                                if (soc > 100) soc = 100;
                                LOG_PRINT("  SoC: %.0f%% | mAh: %.0f\n", soc, mAh);
                            } else {
                                LOG_PRINT("  mAh: %.0f\n", mAh);
                            }
                        }
                    }
                    LOG_PRINT("INA226: %.2fV %.3fA %.2fW\n", get_channel_voltage(3), get_channel_current(3), get_channel_power(3));
                } else {
                    int pn;
                    if (sscanf(line, "display page %d", &pn) == 1 && pn >= 0 && pn <= 4) {
                        LOG_PRINT("=== Display page %d ===\n", pn);
                        if (pn == 0) {
                            LOG_PRINT("  IP: %s | Power: %.1fW | Log: %lu %s\n",
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
                            float v = get_channel_voltage(ch);
                            float i = get_channel_current(ch);
                            float pw = v * i;
                            LOG_PRINT("  Ch%d (%s): %.2fV %.3fA %.2fW\n", ch, name, v, i, pw);
                            float mAh = get_coulomb_mAh(ch);
                            BatteryConfig bat;
                            float soc = -1;
                            if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
                                soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
                                if (soc < 0) soc = 0;
                                if (soc > 100) soc = 100;
                                LOG_PRINT("  SoC: %.0f%% | mAh: %.0f\n", soc, mAh);
                            } else {
                                LOG_PRINT("  mAh: %.0f\n", mAh);
                            }
                        }
                    } else {
                        LOG_PRINTLN("Usage: display page 0-4  |  display all");
                    }
                }
            // Exact-match commands below (no startsWith variants)
            } else if (strcmp(line, "status") == 0) {
                print_status();
            } else if (strcmp(line, "mem") == 0) {
                LOG_PRINT("Free heap: %u bytes\n", ESP.getFreeHeap());
                LOG_PRINT("Heap size: %u bytes\n", ESP.getHeapSize());
                LOG_PRINT("Min free heap: %u bytes\n", ESP.getMinFreeHeap());
                LOG_PRINT("CPU temperature: %.1f C\n", temperatureRead());
                LOG_PRINT("PSRAM: %u bytes\n", ESP.getPsramSize());
            } else if (strcmp(line, "log") == 0) {
                dump_event_log_serial();
            } else if (strcmp(line, "sensors") == 0) {
                SensorSnapshot data = read_sensors();
                print_sensor_data(data);
            } else if (strcmp(line, "switch status") == 0 || strcmp(line, "relay status") == 0) {
                uint8_t count = settings_load_switch_count();
                for (uint8_t i = 0; i < count && i < 8; i++) {
                    SwitchChannel ch;
                    if (settings_load_switch(i, &ch)) {
                        int state = digitalRead(ch.gpio_pin);
                        LOG_PRINT("Switch %d: type=%d pin=%d state=%d name=%s\n",
                            i, ch.type, ch.gpio_pin, state, ch.name);
                    }
                }
            } else if (strncmp(line, "switch rules ", 13) == 0 || strncmp(line, "relay rules ", 12) == 0) {
                int idx;
                const char* p = (line[0] == 's') ? (line + 13) : (line + 12);
                if (sscanf(p, "%d", &idx) == 1 && idx >= 0 && idx < 8) {
                    SwitchRule rule;
                    if (settings_load_switch_rule((uint8_t)idx, &rule)) {
                        LOG_PRINT("Switch %d rule: ch=%u logic=%s min=%u trip=%ums reset=%ums hyst=%.2f en=%d conds=%u\n",
                            idx, rule.channel, switch_logic_name(rule.logic),
                            rule.min_conditions, rule.trip_delay_ms, rule.reset_delay_ms,
                            rule.hysteresis, (int)rule.enabled, rule.condition_count);
                        for (uint8_t i = 0; i < rule.condition_count && i < SC_MAX_CONDITIONS; i++) {
                            const SwitchCondition& c = rule.conditions[i];
                            LOG_PRINT("  [%u] %s %s %.4f (ref_ch=%d)\n",
                                i, switch_condition_kind_name(c.kind),
                                switch_condition_op_name(c.op),
                                c.value, (int)c.ref_channel);
                        }
                    } else {
                        LOG_PRINT("Switch %d rule: not configured\n", idx);
                    }
                } else {
                    LOG_PRINTLN("Usage: switch rules N   (N = 0..7)");
                }
            } else if (strcmp(line, "battery list") == 0) {
                for (uint8_t i = 0; i < MAX_LOGICAL_CHANNELS; i++) {
                    uint8_t pid = battery_channel_profile(i);
                    if (pid == BATTERY_CHANNEL_NO_BINDING) {
                        LOG_PRINT("CH%-2u : (no battery)\n", (unsigned)i);
                    } else {
                        const BatteryChemistryProfile* p = battery_profile_get(pid);
                        if (p) {
                            LOG_PRINT("CH%-2u : profile %u  %s  %.2fV  %.2fAh\n",
                                (unsigned)i, (unsigned)pid, p->name,
                                p->nominal_voltage, p->rated_capacity_Ah);
                        } else {
                            LOG_PRINT("CH%-2u : profile %u (missing)\n", (unsigned)i, (unsigned)pid);
                        }
                    }
                }
            } else if (strncmp(line, "battery bind ", 13) == 0) {
                int ch, pid;
                if (sscanf(line, "battery bind %d %d", &ch, &pid) == 2 &&
                    ch >= 0 && ch < (int)MAX_LOGICAL_CHANNELS) {
                    if (pid < 0) {
                        battery_channel_clear((uint8_t)ch);
                        LOG_PRINT("CH%u cleared\n", (unsigned)ch);
                    } else if (battery_channel_set_profile((uint8_t)ch, (uint8_t)pid)) {
                        LOG_PRINT("CH%u bound to profile %d\n", (unsigned)ch, pid);
                    } else {
                        LOG_PRINTLN("bind failed (invalid id?)");
                    }
                } else {
                    LOG_PRINTLN("Usage: battery bind <ch> <profile_id_or_-1>");
                }
            } else if (strncmp(line, "battery show ", 13) == 0) {
                int ch;
                if (sscanf(line, "battery show %d", &ch) == 1 &&
                    ch >= 0 && ch < (int)MAX_LOGICAL_CHANNELS) {
                    BatteryState st;
                    cycle_counter_get((uint8_t)ch, &st);
                    uint8_t pid = battery_channel_profile((uint8_t)ch);
                    const BatteryChemistryProfile* p = battery_profile_get(pid);
                    LOG_PRINT("CH%u  profile=", (unsigned)ch);
                    if (p) {
                        LOG_PRINT("%u (%s)\n", (unsigned)pid, p->name);
                        LOG_PRINT("  chemistry=%s  rated=%.2fAh  cutoff=%.2fV  float=%.2fV\n",
                            battery_chemistry_name(p->chemistry), p->rated_capacity_Ah,
                            p->cutoff_voltage, p->float_voltage);
                    } else {
                        LOG_PRINTLN("(no profile)");
                    }
                    LOG_PRINT("  SoC=%.1f%%  V=%.3f  I=%.3f\n",
                        st.last_SoC_pct, st.last_V, st.last_I);
                    LOG_PRINT("  cum_Ah_in=%.2f  cum_Ah_out=%.2f  equiv_cycles=%.3f\n",
                        st.cumulative_Ah_in, st.cumulative_Ah_out, st.equivalent_full_cycles);
                    LOG_PRINT("  SoH=%.1f%% (%u samples)\n",
                        st.soh_pct, (unsigned)st.soh_samples);
                } else {
                    LOG_PRINTLN("Usage: battery show <ch>");
                }
            } else if (strncmp(line, "battery profile show ", 21) == 0) {
                int pid;
                if (sscanf(line, "battery profile show %d", &pid) == 1 && pid >= 0 && pid < BATTERY_MAX_PROFILES) {
                    const BatteryChemistryProfile* p = battery_profile_get((uint8_t)pid);
                    if (p) {
                        LOG_PRINT("Profile %u: %s\n", (unsigned)pid, p->name);
                        LOG_PRINT("  chemistry=%s  nom=%.2fV  rated=%.2fAh  C=%.2f\n",
                            battery_chemistry_name(p->chemistry), p->nominal_voltage,
                            p->rated_capacity_Ah, p->c_rating);
                        LOG_PRINT("  cutoff=%.2fV  float=%.2fV  eff=%.2f  cycles=%u\n",
                            p->cutoff_voltage, p->float_voltage, p->charge_efficiency,
                            (unsigned)p->cycle_life_rated);
                        LOG_PRINT("  soc_min=%.1f  soc_max=%.1f\n", p->min_soc_pct, p->max_soc_pct);
                    } else {
                        LOG_PRINTLN("(empty slot)");
                    }
                } else {
                    LOG_PRINTLN("Usage: battery profile show <id>");
                }
            } else if (strncmp(line, "battery profile reset ", 22) == 0) {
                int pid;
                if (sscanf(line, "battery profile reset %d", &pid) == 1 &&
                    pid >= 0 && pid < (int)BATTERY_BUILTIN_PROFILE_COUNT) {
                    battery_profile_reset_builtin((uint8_t)pid);
                    LOG_PRINT("Profile %u reset to built-in defaults\n", pid);
                } else {
                    LOG_PRINTLN("Usage: battery profile reset <0..3>");
                }
            } else if (strncmp(line, "battery profile delete ", 23) == 0) {
                int pid;
                if (sscanf(line, "battery profile delete %d", &pid) == 1 &&
                    pid >= (int)BATTERY_BUILTIN_PROFILE_COUNT && pid < BATTERY_MAX_PROFILES) {
                    if (battery_profile_delete((uint8_t)pid)) {
                        LOG_PRINT("Profile %u deleted\n", pid);
                    } else {
                        LOG_PRINTLN("delete failed");
                    }
                } else {
                    LOG_PRINTLN("Usage: battery profile delete <4..15>");
                }
            } else if (strncmp(line, "battery profile set ", 20) == 0) {
                int pid;
                char field[24];
                char value_str[32];
                if (sscanf(line, "battery profile set %d %23s %31s", &pid, field, value_str) == 3 &&
                    pid >= 0 && pid < BATTERY_MAX_PROFILES) {
                    const BatteryChemistryProfile* cur = battery_profile_get((uint8_t)pid);
                    if (!cur) {
                        LOG_PRINTLN("(empty slot — nothing to modify)");
                    } else {
                        BatteryChemistryProfile p = *cur;
                        float v = atof(value_str);
                        if (strcmp(field, "name") == 0) {
                            strlcpy(p.name, value_str, sizeof(p.name));
                        } else if (strcmp(field, "chemistry") == 0) {
                            p.chemistry = (uint8_t)atoi(value_str);
                        } else if (strcmp(field, "nominal_voltage") == 0) {
                            p.nominal_voltage = v;
                        } else if (strcmp(field, "rated_capacity_Ah") == 0) {
                            p.rated_capacity_Ah = v;
                        } else if (strcmp(field, "c_rating") == 0) {
                            p.c_rating = v;
                        } else if (strcmp(field, "cutoff_voltage") == 0) {
                            p.cutoff_voltage = v;
                        } else if (strcmp(field, "float_voltage") == 0) {
                            p.float_voltage = v;
                        } else if (strcmp(field, "charge_efficiency") == 0) {
                            p.charge_efficiency = v;
                        } else if (strcmp(field, "cycle_life_rated") == 0) {
                            p.cycle_life_rated = (uint16_t)atoi(value_str);
                        } else if (strcmp(field, "min_soc_pct") == 0) {
                            p.min_soc_pct = v;
                        } else if (strcmp(field, "max_soc_pct") == 0) {
                            p.max_soc_pct = v;
                        } else {
                            LOG_PRINTLN("Unknown field. Valid: name chemistry nominal_voltage rated_capacity_Ah c_rating cutoff_voltage float_voltage charge_efficiency cycle_life_rated min_soc_pct max_soc_pct");
                            return;
                        }
                        battery_profile_set(&p);
                        LOG_PRINT("Profile %u: %s = %s\n", pid, field, value_str);
                    }
                } else {
                    LOG_PRINTLN("Usage: battery profile set <id> <field> <value>");
                }
            } else if (strncmp(line, "battery reset ", 14) == 0) {
                int ch;
                if (sscanf(line, "battery reset %d", &ch) == 1 && ch >= 0 && ch < (int)MAX_LOGICAL_CHANNELS) {
                    cycle_counter_reset((uint8_t)ch);
                    reset_coulomb_counter((uint8_t)ch);
                    LOG_PRINT("CH%u battery state reset\n", (unsigned)ch);
                } else {
                    LOG_PRINTLN("Usage: battery reset <ch>");
                }
            } else if (strncmp(line, "cycle show ", 11) == 0) {
                int ch;
                if (sscanf(line, "cycle show %d", &ch) == 1 && ch >= 0 && ch < (int)MAX_LOGICAL_CHANNELS) {
                    BatteryState st;
                    cycle_counter_get((uint8_t)ch, &st);
                    LOG_PRINT("CH%u cycles: equiv=%.3f  cum_in=%.2fAh  cum_out=%.2fAh  SoC=%.1f%%\n",
                        (unsigned)ch, st.equivalent_full_cycles,
                        st.cumulative_Ah_in, st.cumulative_Ah_out, st.last_SoC_pct);
                } else {
                    LOG_PRINTLN("Usage: cycle show <ch>");
                }
            } else if (strcmp(line, "flush log") == 0) {
                size_t flushed = 0;
                uint8_t batch[512];
                size_t n;
                while ((n = log_pop_batch(batch, sizeof(batch))) > 0) {
                    flushed += n;
                }
                LOG_PRINT("Flushed %u bytes from log buffer\n", (unsigned)flushed);
            } else if (strcmp(line, "i2c_scan") == 0) {
                Wire.begin(I2C_SDA, I2C_SCL);
                LOG_PRINTLN("I2C scan:");
                for (uint8_t addr = 1; addr < 127; addr++) {
                    Wire.beginTransmission(addr);
                    if (Wire.endTransmission() == 0) {
                        LOG_PRINT("  0x%X\n", addr);
                    }
                }
                LOG_PRINTLN("done");
            } else if (strcmp(line, "discover_sensors") == 0) {
                discover_sensors();
            } else if (strcmp(line, "test all switches") == 0 || strcmp(line, "test all relays") == 0) {
                LOG_PRINTLN("Testing all switches in sequence...");
                uint8_t count = settings_load_switch_count();
                for (int i = 0; i < count && i < 8; i++) {
                    SwitchChannel ch;
                    if (settings_load_switch(i, &ch)) {
                        LOG_PRINT("Switch %d (GPIO %d)...\n", i, ch.gpio_pin);
                        // switch_pulse is non-blocking, so wait for this
                        // switch's pulse to finish before starting the next
                        // to keep the test sequential.
                        switch_pulse((uint8_t)i, 1000);
                        while (switch_pulse_active((uint8_t)i)) {
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
                LOG_PRINTLN("All switches tested.");
            } else if (strcmp(line, "test all sensors") == 0) {
                SensorSnapshot d = read_sensors();
                LOG_PRINTLN("All sensor channels:");
                for (int i = 0; i < 3; i++) {
                    LOG_PRINT("  CH%d: %.3fV  %.3fA\n", i, get_channel_voltage(i), get_channel_current(i));
                }
            } else if (strcmp(line, "test display") == 0) {
                LOG_PRINTLN("Display test: OLED should show cycling pages");
                extern void init_display();
                extern void update_display(const SensorSnapshot&, const char*, float);
                SensorSnapshot d = read_sensors();
                for (int i = 0; i < 5; i++) {
                    update_display(d, "192.168.1.1", 123.4f);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                LOG_PRINTLN("Display test done.");
            } else if (strcmp(line, "switch auto on") == 0 || strcmp(line, "relay auto on") == 0) {
                switch_set_auto(true);
                LOG_PRINTLN("Switch auto-trip ENABLED");
            } else if (strcmp(line, "switch auto off") == 0 || strcmp(line, "relay auto off") == 0) {
                switch_set_auto(false);
                LOG_PRINTLN("Switch auto-trip DISABLED");
            } else if (strcmp(line, "factory_reset") == 0) {
                LOG_PRINTLN("Wiping NVS and rebooting...");
                vTaskDelay(pdMS_TO_TICKS(500));
                settings_factory_reset();
                mark_clean_shutdown();
                ESP.restart();
            } else if (strcmp(line, "calibrate_baseline") == 0) {
                sensor_calibrate_baseline();
                LOG_PRINTLN("Baseline recalibration started — collecting new baseline over next 10 ticks");
            } else if (strcmp(line, "ble_on") == 0) {
                // The WiFi-state machine deinits the BLE stack to free ~50KB
                // heap for TLS. The first reconnect re-initialises
                // automatically, but if the user wants to provision via BLE
                // while WiFi is up, they need a way to bring it back.
                // init_ble_provisioner() is a no-op if already initialised.
                init_ble_provisioner();
                start_ble_advertising();
                LOG_PRINTLN("BLE (re-)initialised and advertising");
            } else if (strcmp(line, "wifi_show") == 0) {
                char ssid[64] = "", pass[64] = "";
                if (settings_load_wifi(ssid, pass, sizeof(ssid))) {
                    LOG_PRINT("WiFi SSID: %s\n", ssid);
                } else {
                    LOG_PRINTLN("WiFi: not configured");
                }
            } else if (strcmp(line, "supabase_show") == 0) {
                char url[128] = "", anon_key[128] = "", device_key[64] = "", api_key[64] = "";
                bool has_url = settings_load_supabase_url(url, sizeof(url));
                bool has_akey = settings_load_supabase_anon_key(anon_key, sizeof(anon_key));
                bool has_dkey = settings_load_supabase_device_key(device_key, sizeof(device_key));
                bool has_apikey = settings_load_supabase_api_key(api_key, sizeof(api_key));
                if (has_url) LOG_PRINT("Supabase URL: %s\n", url);
                else LOG_PRINTLN("Supabase URL: not set");
                if (has_akey) LOG_PRINT("Supabase anon key: %s\n", anon_key);
                if (has_dkey) LOG_PRINT("Device key: %s\n", device_key);
                if (has_apikey) LOG_PRINT("Device API key: %s\n", api_key);
                if (!has_url && !has_akey) LOG_PRINTLN("Supabase: not configured");
            } else if (strcmp(line, "virtual_channel show") == 0) {
                LOG_PRINTLN("Virtual channel configs:");
                for (int ch = 0; ch < 4; ch++) {
                    VirtualChannelConfig vc;
                    if (settings_load_virtual_channel(ch, &vc)) {
                        LOG_PRINT("  CH%d: V=src%d:idx%d I=src%d:idx%d\n",
                            ch, vc.voltage_src, vc.voltage_idx, vc.current_src, vc.current_idx);
                    } else {
                        LOG_PRINT("  CH%d: not configured\n", ch);
                    }
                }
            } else if (strcmp(line, "reboot") == 0) {
                LOG_PRINTLN("Rebooting...");
                vTaskDelay(pdMS_TO_TICKS(100));
                mark_clean_shutdown();
                ESP.restart();
            } else if (strcmp(line, "serial1peek") == 0) {
#if ENABLE_SERIAL1
                char buf[80];
                int count = 0;
                while (serial1_available() > 0 && count < 5) {
                    if (serial1_read_line(buf, sizeof(buf))) {
                        LOG_PRINT("[S1] %s\n", buf);
                        count++;
                    }
                }
                if (count == 0) LOG_PRINTLN("S1: no data");
#else
                LOG_PRINTLN("Serial1 disabled");
#endif
            } else if (strcmp(line, "help") == 0) {
                LOG_PRINTLN("Commands:");
                LOG_PRINTLN("  status              — IP, log entries, coulomb/energy/SoC");
                LOG_PRINTLN("  mem                 — free heap bytes, CPU temperature");
                LOG_PRINTLN("  sensors             — all sensor readings");
                LOG_PRINTLN("  switch/relay status  — switch GPIO states");
                LOG_PRINTLN("  switch/relay rules N — print parsed rule + conditions");
                LOG_PRINTLN("  switch/relay N 0/1   — manual override switch N (0=off, 1=on)");
                LOG_PRINTLN("  test switch/relay N  — pulse switch N for 3s (auto-reset)");
                LOG_PRINTLN("  test all switches/relays — pulse each switch 1s in sequence");
                LOG_PRINTLN("  test sensor N       — read sensor CH N once (0-2)");
                LOG_PRINTLN("  test all sensors    — read all sensor channels");
                LOG_PRINTLN("  test display        — cycle OLED pages 5x");
                LOG_PRINTLN("  display page N     — print display page N (0-4)");
                LOG_PRINTLN("  display all         — print all display pages");
                LOG_PRINTLN("  switch/relay auto on  — enable auto-trip (off by default)");
                LOG_PRINTLN("  switch/relay auto off — disable auto-trip");
                LOG_PRINTLN("  factory_reset       — wipe all NVS settings, reboot");
                LOG_PRINTLN("  reset coulomb N     — reset coulomb counter CH N");
                LOG_PRINTLN("  reset energy N      — reset energy counter CH N");
                LOG_PRINTLN("  battery list        — show channel → profile binding");
                LOG_PRINTLN("  battery bind N id   — bind CH N to profile id (or -1 to clear)");
                LOG_PRINTLN("  battery show N      — show battery state for CH N");
                LOG_PRINTLN("  battery profile show id   — show chemistry profile id");
                LOG_PRINTLN("  battery profile set id field value — modify profile field");
                LOG_PRINTLN("  battery profile reset id  — restore built-in defaults (id 0..3)");
                LOG_PRINTLN("  battery profile delete id — delete custom profile (id 4..15)");
                LOG_PRINTLN("  battery reset N     — reset cycle counter for CH N");
                LOG_PRINTLN("  cycle show N        — show cycle counter for CH N");
                LOG_PRINTLN("  flush log           — flush RAM log buffer");
                LOG_PRINTLN("  i2c_scan            — scan I2C bus for devices");
                LOG_PRINTLN("  discover_sensors    — auto-detect INA226/BL0939 sensors");
                LOG_PRINTLN("  shunt N ohms        — set shunt resistance for CH N (0 clears)");
                LOG_PRINTLN("  shunt show          — show current shunt settings");
                LOG_PRINTLN("  vratio N ratio      — set voltage divider ratio for CH N (0 clears)");
                LOG_PRINTLN("  vratio show         — show current voltage ratios");
                LOG_PRINTLN("  resistor N r_high r_low — set R values, ratio auto-computed");
                LOG_PRINTLN("  resistor show        — show resistor values per channel");
                LOG_PRINTLN("  cal N type value    — set calibration (type: 0=vo_mv, 1=vg, 2=co_ma, 3=cg)");
                LOG_PRINTLN("  cal show            — show all channel calibration values");
                LOG_PRINTLN("  calibrate_baseline  — restart baseline noise calibration (10 ticks)");
                LOG_PRINTLN("  ble_on              — restart BLE advertising (for re-provisioning)");
                LOG_PRINTLN("  serial1peek         — dump up to 5 lines from Serial1");
                LOG_PRINTLN("  wifi_show          — show current WiFi SSID");
                LOG_PRINTLN("  wifi_ssid <ssid>  — set WiFi SSID");
                LOG_PRINTLN("  wifi_pass <pass>  — set WiFi password");
                LOG_PRINTLN("  set_wifi <ssid> <pass> — set both SSID and password");
                LOG_PRINTLN("  supabase_show     — show Supabase config");
                LOG_PRINTLN("  supabase <url> <anon_key> <device_api_key> <device_key>");
                LOG_PRINTLN("     device_api_key = UUID from Supabase devices table (not sb_secret_...)");
                LOG_PRINTLN("  reboot              — reboot the device");
                LOG_PRINTLN("  help                — this list");
            } else if (len > 0) {
                LOG_PRINTLN("Unknown command. Type 'help'.");
            }
        } else {
            if (len < 255) line[len++] = c;
        }
    }
#endif // HAS_SERIAL
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino setup — create FreeRTOS tasks
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
#if HAS_SERIAL
    Serial.begin(115200);
    Serial.setTxBufferSize(4096);
    while (!Serial) { ; }
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));
    LOG_PRINTLN("Power Monitor v2 starting...");

    init_settings();
    init_event_log();
    init_device_identity();
    LOG_PRINT("Device: %s rev %s (crashes: %u)\n",
        get_device_serial(), get_device_hw_rev(), (unsigned)get_crash_count());

    // Safe mode: if the device has crashed 5+ times, skip WiFi/BLE init
    // to avoid re-triggering the fault loop. The user must factory-reset
    // or flash new firmware to recover.
    if (get_crash_count() >= 5) {
        LOG_PRINTLN("*** SAFE MODE — crash count >= 5, skipping network init ***");
        LOG_PRINTLN("Factory reset or reflash to recover.");
        // Still init sensors and logger so the serial console works
        init_sensors();
        init_data_logger();
        init_switches();
        init_ui();
        ui_set_heartbeat(true);
        init_core_shared();
        wdt_init();
        // Create only the sensor task (no network/BLE)
        xTaskCreatePinnedToCore(sensorTask, "sensorTask", 4096, NULL, 5, NULL, 1);
        vTaskDelete(NULL);  // delete setup task, sensorTask runs
        return;
    }

    init_sensors();
    init_display();
    log_set_epoch(get_epoch_time());
    init_data_logger();
    init_coulomb_counter();
    init_energy_counter();
    init_battery_profiles();
    init_battery_bindings();
    init_battery_states();
    init_cycle_counter();
    init_switches();
    init_ui();
    ui_set_heartbeat(true);
    init_core_shared();
    init_ota_client();

    LOG_PRINT("Free heap before BLE: %u bytes\n", ESP.getFreeHeap());
    LOG_PRINT("Min free heap: %u bytes\n", ESP.getMinFreeHeap());
    LOG_PRINT("Largest alloc: %u bytes\n", ESP.getMaxAllocHeap());
    init_ble_provisioner();  // BLE stack needs ~60-80KB contiguous heap

    // Initialize the TWDT BEFORE creating tasks so each task can immediately
    // subscribe via esp_task_wdt_add(NULL) on its first iteration.
    wdt_init();

    // Network task: 16 KB stack (was 16384). Network has the deepest stack
    // (PubSubClient, mbedTLS, large static buffers, MQTT JSON, base64 encode,
    // HTTP response drain buffers) — keep generous. On uniprocessor builds
    // (e.g. esp32c3 with CONFIG_FREERTOS_UNICORE=1) the Arduino xTaskCreate
    // wrapper routes everything to core 0; we still pass the preferred core
    // for clarity on dual-core boards.
#if CONFIG_FREERTOS_UNICORE
    xTaskCreatePinnedToCore(networkTask, "Network", 16384, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(sensorTask,    "Sensor",   4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(uiTask,        "UI",       2048, NULL, 2, NULL, 0);
#else
    xTaskCreatePinnedToCore(networkTask, "Network", 16384, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(sensorTask,    "Sensor",   4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(uiTask,        "UI",       2048, NULL, 2, NULL, 1);
#endif

    LOG_PRINTLN("Type 'help' for serial commands");
}

void loop() {
    // Not used — all work is in FreeRTOS tasks
    static uint32_t last_heap_check = 0;
    if (millis() - last_heap_check >= 5000) {
        last_heap_check = millis();
        LOG_PRINT("[MEM] free=%u min=%u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    }
    handle_serial_cli();
    vTaskDelay(pdMS_TO_TICKS(10));
}