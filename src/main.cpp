#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "sensor_manager.h"
#include "display_manager.h"
#include "connectivity_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "relay_controller.h"
#include "settings_manager.h"
#include "ble_provisioner.h"
#include "serial1_manager.h"

static unsigned long last1s = 0;
static unsigned long last5min = 0;
static unsigned long last5s = 0;

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
    Serial.printf("IP: %s | Entries: %lu | Overflow: %d\n", get_local_ip_str(), log_entries_count(), log_has_overflow_file());
    for (int ch = 0; ch < 4; ch++) {
        float mAh = get_coulomb_mAh(ch);
        BatteryConfig bat;
        float soc = -1;
        if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
            soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
            if (soc < 0) soc = 0;
            if (soc > 100) soc = 100;
        }
        if (soc >= 0) {
            Serial.printf("Ch%d: %.1fmAh SoC:%.1f%%\n", ch, mAh, soc);
        } else {
            Serial.printf("Ch%d: %.1fmAh\n", ch, mAh);
        }
    }
}

static void handle_serial_cli() {
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (line.length() > 0) {
                line.trim();
                if (line == "status") {
                    print_status();
                } else if (line == "sensors") {
                    SensorData data = read_sensors();
                    print_sensor_data(data);
                } else if (line == "relay status") {
                    uint8_t count = settings_load_relay_count();
                    for (uint8_t i = 0; i < count; i++) {
                        RelayRule rt;
                        if (settings_load_relay(i, &rt)) {
                            int state = digitalRead(rt.gpio_pin);
                            Serial.printf("Relay %d: ch=%d pin=%d state=%d\n", i, rt.channel, rt.gpio_pin, state);
                        }
                    }
                } else if (line.startsWith("relay ")) {
                    int idx, st;
                    if (sscanf(line.c_str(), "relay %d %d", &idx, &st) == 2) {
                        RelayRule rt;
                        if (settings_load_relay(idx, &rt)) {
                            digitalWrite(rt.gpio_pin, st ? HIGH : LOW);
                            Serial.printf("Relay %d set to %d\n", idx, st);
                        } else {
                            Serial.println("Relay not found");
                        }
                    }
                } else if (line.startsWith("reset coulomb ")) {
                    int ch;
                    if (sscanf(line.c_str(), "reset coulomb %d", &ch) == 1 && ch >= 0 && ch <= 3) {
                        reset_coulomb_counter(ch);
                        Serial.printf("Coulomb counter ch%d reset\n", ch);
                    }
                } else if (line == "flush log") {
                    size_t flushed = 0;
                    uint8_t batch[512];
                    size_t n;
                    while ((n = log_pop_batch(batch, sizeof(batch))) > 0) {
                        flushed += n;
                    }
                    Serial.printf("Flushed %u bytes from log buffer\n", (unsigned)flushed);
                } else if (line == "i2c_scan") {
                    Wire.begin(I2C_SDA, I2C_SCL);
                    Serial.println("I2C scan:");
                    for (uint8_t addr = 1; addr < 127; addr++) {
                        Wire.beginTransmission(addr);
                        if (Wire.endTransmission() == 0) {
                            Serial.print("  0x"); Serial.println(addr, HEX);
                        }
                    }
                    Serial.println("done");
                } else if (line.startsWith("test relay ")) {
                    int idx;
                    if (sscanf(line.c_str(), "test relay %d", &idx) == 1 && idx >= 0 && idx <= 3) {
                        RelayRule rt;
                        if (settings_load_relay(idx, &rt)) {
                            Serial.printf("Testing relay %d on GPIO %d...\n", idx, rt.gpio_pin);
                            Serial.println("Activating 3s...");
                            digitalWrite(rt.gpio_pin, HIGH);
                            delay(3000);
                            digitalWrite(rt.gpio_pin, LOW);
                            Serial.println("Relay deactivated.");
                        } else {
                            Serial.println("Relay not configured. Use 'relay N 0/1' to manual override.");
                        }
                    } else {
                        Serial.println("Usage: test relay 0-3");
                    }
                } else if (line.startsWith("test sensor ")) {
                    int ch;
                    if (sscanf(line.c_str(), "test sensor %d", &ch) == 1 && ch >= 0 && ch <= 2) {
                        SensorData d = read_sensors();
                        Serial.printf("Sensor CH%d: %.3fV, %.3fA\n", ch, d.ads1115_volts[ch], d.ina3221_current[ch]);
                        Serial.printf("  raw shunt voltage: %.2fmV\n", ina3221_getShuntVoltage(ch) * 1000.0f);
                    } else {
                        Serial.println("Usage: test sensor 0-2");
                    }
                } else if (line.startsWith("shunt ")) {
                    int ch; float ohms;
                    if (sscanf(line.c_str(), "shunt %d %f", &ch, &ohms) == 2 && ch >= 0 && ch <= 3) {
                        if (ohms <= 0.0f) {
                            Serial.printf("CH%d shunt cleared (using default)\n", ch);
                        } else {
                            Serial.printf("CH%d shunt set to %.6f Ohm\n", ch, ohms);
                        }
                        settings_save_shunt(ch, ohms);
                    } else {
                        Serial.println("Usage: shunt N ohms (e.g. shunt 0 0.0003) or shunt N 0 to clear");
                    }
                } else if (line.startsWith("shunt show")) {
                    for (int ch = 0; ch < 3; ch++) {
                        float s; bool ok = settings_load_shunt(ch, &s);
                        Serial.printf("CH%d shunt: %s\n", ch, ok ? String(s, 6).c_str() : "(default)");
                    }
                } else if (line.startsWith("vratio ")) {
                    int ch; float ratio;
                    if (sscanf(line.c_str(), "vratio %d %f", &ch, &ratio) == 2 && ch >= 0 && ch <= 2) {
                        if (ratio <= 0.0f) {
                            Serial.printf("CH%d vratio cleared (using config default %.4f)\n", ch, VOLT_RATIO_CH0);
                        } else {
                            Serial.printf("CH%d vratio set to %.4f\n", ch, ratio);
                        }
                        settings_save_volt_ratio(ch, ratio);
                    } else {
                        Serial.println("Usage: vratio N ratio (e.g. vratio 2 3.521) or vratio N 0 to clear");
                    }
                } else if (line.startsWith("vratio show")) {
                    for (int ch = 0; ch < 3; ch++) {
                        float r; bool ok = settings_load_volt_ratio(ch, &r);
                        float def = (ch == 0) ? VOLT_RATIO_CH0 : (ch == 1) ? VOLT_RATIO_CH1 : VOLT_RATIO_CH2;
                        Serial.printf("CH%d vratio: %s\n", ch, ok ? String(r, 4).c_str() : String("default:") + String(def, 4));
                    }
                } else if (line.startsWith("resistor ")) {
                    int ch; float rh, rl;
                    if (sscanf(line.c_str(), "resistor %d %f %f", &ch, &rh, &rl) == 3 && ch >= 0 && ch <= 2) {
                        if (rh <= 0.0f || rl <= 0.0f) {
                            Serial.printf("CH%d resistors cleared\n", ch);
                        } else {
                            float ratio = (rh + rl) / rl;
                            Serial.printf("CH%d R=%.0f+%.0f -> ratio=%.4f\n", ch, rh, rl, ratio);
                        }
                        settings_save_resistors(ch, rh, rl);
                    } else {
                        Serial.println("Usage: resistor N r_high r_low (e.g. resistor 2 900000 68000) or all 0 to clear");
                    }
                } else if (line.startsWith("resistor show")) {
                    for (int ch = 0; ch < 3; ch++) {
                        float rh, rl; bool ok = settings_load_resistors(ch, &rh, &rl);
                        Serial.printf("CH%d resistors: %s\n", ch, ok ? (String(rh, 0) + "+" + rl + " = " + String((rh+rl)/rl, 4)).c_str() : "(not set)");
                    }
                } else if (line.startsWith("cal ")) {
                    int ch, type; float value;
                    if (sscanf(line.c_str(), "cal %d %d %f", &ch, &type, &value) == 3 && ch >= 0 && ch <= 2 && type >= 0 && type <= 3) {
                        sensor_set_calibration(ch, type, value);
                        Serial.printf("CH%d cal type=%d value=%.4f saved\n", ch, type, value);
                    } else {
                        Serial.println("Usage: cal N type value — type: 0=volt_offset_mv, 1=volt_gain, 2=curr_offset_ma, 3=curr_gain");
                    }
                } else if (line.startsWith("cal show")) {
                    for (int ch = 0; ch < 3; ch++) {
                        float vo, vg, co, cg;
                        sensor_get_calibration(ch, &vo, &vg, &co, &cg);
                        Serial.printf("CH%d: vo=%.2fmV vg=%.4f co=%.2fmA cg=%.4f\n", ch, vo, vg, co, cg);
                    }
                } else if (line.startsWith("serial1peek")) {
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
                    Serial.println("Display test: OLED should show cycling pages");
                    extern void init_display();
                    extern void update_display(const SensorData&, const char*, float);
                    SensorData d = read_sensors();
                    for (int i = 0; i < 5; i++) {
                        update_display(d, "192.168.1.1", 123.4f);
                        delay(1000);
                    }
                    Serial.println("Display test done.");
                } else if (line == "test all relays") {
                    Serial.println("Testing all relays in sequence...");
                    for (int i = 0; i < 4; i++) {
                        RelayRule rt;
                        if (settings_load_relay(i, &rt)) {
                            Serial.printf("Relay %d (GPIO %d)...\n", i, rt.gpio_pin);
                            digitalWrite(rt.gpio_pin, HIGH);
                            delay(1000);
                            digitalWrite(rt.gpio_pin, LOW);
                            delay(500);
                        }
                    }
                    Serial.println("All relays tested.");
                } else if (line == "relay auto on") {
                    relay_set_auto(true);
                    Serial.println("Relay auto-trip ENABLED");
                } else if (line == "relay auto off") {
                    relay_set_auto(false);
                    Serial.println("Relay auto-trip DISABLED");
                } else if (line == "factory_reset") {
                    Serial.println("Wiping NVS and rebooting...");
                    delay(500);
                    settings_factory_reset();
                    ESP.restart();
                } else if (line == "test all sensors") {
                    SensorData d = read_sensors();
                    Serial.println("All sensor channels:");
                    for (int i = 0; i < 3; i++) {
                        Serial.printf("  CH%d: %.3fV  %.3fA\n", i, d.ads1115_volts[i], d.ina3221_current[i]);
                    }
                } else if (line == "help") {
                    Serial.println("Commands:");
                    Serial.println("  status              — IP, log entries, coulomb/SoC");
                    Serial.println("  sensors             — all sensor readings");
                    Serial.println("  relay status        — relay GPIO states");
                    Serial.println("  relay N 0/1         — manual override relay N (0=off, 1=on)");
                    Serial.println("  test relay N        — pulse relay N for 3s (auto-reset)");
                    Serial.println("  test all relays     — pulse each relay 1s in sequence");
                    Serial.println("  test sensor N       — read sensor CH N once (0-2)");
                    Serial.println("  test all sensors    — read all sensor channels");
                    Serial.println("  test display        — cycle OLED pages 5x");
                    Serial.println("  relay auto on       — enable auto-trip (off by default)");
                    Serial.println("  relay auto off      — disable auto-trip");
                    Serial.println("  factory_reset       — wipe all NVS settings, reboot");
                    Serial.println("  reset coulomb N     — reset coulomb counter CH N");
                    Serial.println("  flush log           — flush RAM log buffer");
                    Serial.println("  i2c_scan            — scan I2C bus for devices");
                    Serial.println("  shunt N ohms       — set shunt resistance for CH N (0 clears)");
                    Serial.println("  shunt show         — show current shunt settings");
                    Serial.println("  vratio N ratio     — set voltage divider ratio for CH N (0 clears)");
                    Serial.println("  vratio show        — show current voltage ratios");
                    Serial.println("  resistor N r_high r_low — set R values, ratio auto-computed");
                    Serial.println("  resistor show       — show resistor values per channel");
                    Serial.println("  cal N type value    — set calibration (type: 0=vo_mv, 1=vg, 2=co_ma, 3=cg)");
                    Serial.println("  cal show            — show all channel calibration values");
                    Serial.println("  help                — this list");
                } else if (line.length() > 0) {
                    Serial.println("Unknown command. Type 'help'.");
                }
                line = "";
            }
        } else {
            if (line.length() < 63) line += c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }
    delay(1000);
    Serial.println("Power Monitor v2 starting...");

    init_settings();
    init_sensors();
    init_display();
    init_connectivity();
    log_set_epoch(get_epoch_time());
    init_data_logger();
    init_coulomb_counter();
    init_relays();
    init_ble_provisioner();
    init_serial1();

    Serial.println("Type 'help' for serial commands");
}

void loop() {
    unsigned long now = millis();

    SensorData data;
    if (now - last1s >= FAST_SAMPLE_INTERVAL_MS) {
        last1s = now;
        data = read_sensors();
        log_sample(data, now);
        update_coulomb_counter(data, FAST_SAMPLE_INTERVAL_MS / 1000.0f);
        evaluate_relays(data);
    }

    if (now - last5min >= 300000) {
        last5min = now;
        publish_data_supabase(data);
    }

    if (now - last5s >= SAMPLE_INTERVAL_MS) {
        last5s = now;
        data = read_sensors();
        float total_power = 0;
        for (int ch = 0; ch < 3; ch++) total_power += data.ads1115_volts[ch] * data.ina3221_current[ch];
        total_power += data.ina226_power;
        publish_data(data);
        update_display(data, get_local_ip_str(), total_power);
    }

    loop_connectivity();
    loop_ble_provisioner();
    loop_serial1();
    handle_serial_cli();
    delay(10);
}
