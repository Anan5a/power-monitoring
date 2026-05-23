#include <Arduino.h>
#include "config.h"
#include "sensor_manager.h"
#include "display_manager.h"
#include "connectivity_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "relay_controller.h"
#include "settings_manager.h"
#include "ble_provisioner.h"

static unsigned long last1s = 0;
static unsigned long last5min = 0;
static unsigned long last5s = 0;

static void print_sensor_data(const SensorData& data) {
    for (int i = 0; i < 3; i++) {
        Serial.printf("CH%d: %.2fV %.3fA\n", i, data.ina3221_busV[i], data.ina3221_current[i]);
    }
    Serial.printf("INA226: %.2fV %.3fA %.2fW\n", data.ina226_busV, data.ina226_current, data.ina226_power);
    Serial.printf("ADC: %.3fV %.3fV %.3fV %.3fV\n", data.ads1115_volts[0], data.ads1115_volts[1], data.ads1115_volts[2], data.ads1115_volts[3]);
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
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

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
                digitalWrite(rt.gpio_pin, st ? (rt.active_high ? HIGH : LOW) : (rt.active_high ? LOW : HIGH));
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
    } else if (line == "help") {
        Serial.println("Commands: status, sensors, relay status, relay N 0/1, reset coulomb N, flush log, help");
    } else {
        Serial.println("Unknown command. Type 'help'.");
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
        for (int ch = 0; ch < 3; ch++) total_power += data.ina3221_busV[ch] * data.ina3221_current[ch];
        total_power += data.ina226_power;
        publish_data(data);
        update_display(data, get_local_ip_str(), total_power);
    }

    loop_connectivity();
    loop_ble_provisioner();
    handle_serial_cli();
    delay(10);
}
