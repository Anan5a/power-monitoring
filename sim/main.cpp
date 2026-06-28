#include "Arduino.h"
#include "sensor_manager.h"
#include "data_logger.h"
#include "coulomb_counter.h"
#include "energy_counter.h"
#include "settings_manager.h"
#include "ina226_mock.h"
#include "bl0939_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static SensorSnapshot build_sensor_snapshot(float t) {
    SensorSnapshot snap{};
    BL0939Mock bl0939;
    INA226Mock ina226;

    // Pod 0..2: legacy INA3221 logical channels (single-channel pods)
    for (int ch = 0; ch < 3; ch++) {
        uint8_t bl_ch = (ch < 2) ? 0 : 1;
        BL0939Channel bl = bl0939.sampleChannel(bl_ch, t + ch * 0.05f);
        snap.pods[ch].id = ch;
        snap.pods[ch].type = POD_INA226;
        snap.pods[ch].num_channels = 1;
        snap.pods[ch].channels[0].pod_id = ch;
        snap.pods[ch].channels[0].pod_channel = 0;
        snap.pods[ch].channels[0].voltage = bl.voltage_V;
        snap.pods[ch].channels[0].current = bl.current_A;
        snap.pods[ch].channels[0].power = bl.voltage_V * bl.current_A;
    }

    // Pod 3: INA226
    INA226Reading ina = ina226.sample(t);
    snap.pods[3].id = 3;
    snap.pods[3].type = POD_INA226;
    snap.pods[3].num_channels = 1;
    snap.pods[3].channels[0].pod_id = 3;
    snap.pods[3].channels[0].pod_channel = 0;
    snap.pods[3].channels[0].voltage = ina.busVoltage_V;
    snap.pods[3].channels[0].current = ina.current_A;
    snap.pods[3].channels[0].power = ina.power_W;

    snap.num_pods = 4;
    snap.total_logical_channels = 4;
    snap.timestamp_ms = millis();

    return snap;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    srand(static_cast<unsigned>(time(nullptr)));

    init_settings();
    init_data_logger();
    init_coulomb_counter();
    init_energy_counter();

    uint32_t start_ms = millis();

    for (int iter = 0; iter < 60; iter++) {
        float t = (millis() - start_ms) / 1000.0f;
        SensorSnapshot snap = build_sensor_snapshot(t);
        sim_set_last_snapshot(snap);

        uint32_t now = millis();
        log_sample(snap, now);
        update_coulomb_counter(snap, 1.0f);
        update_energy_counter(snap, 1.0f);

        if ((iter + 1) % 5 == 0) {
            printf("{\"iter\":%d,\"ts\":%u,\"v\":[", iter, static_cast<unsigned>(now));
            for (int ch = 0; ch < 3; ch++) {
                printf("%.3f%s", get_channel_voltage(snap, ch), ch < 2 ? "," : "");
            }
            printf("],\"i\":[");
            for (int ch = 0; ch < 3; ch++) {
                printf("%.3f%s", get_channel_current(snap, ch), ch < 2 ? "," : "");
            }
            printf("],\"ina226\":{\"v\":%.3f,\"i\":%.3f,\"p\":%.3f},",
                   get_channel_voltage(snap, 3), get_channel_current(snap, 3), get_channel_power(snap, 3));
            printf("\"coulomb_mAh\":[");
            for (int ch = 0; ch < 4; ch++) {
                printf("%.6f%s", get_coulomb_mAh(ch), ch < 3 ? "," : "");
            }
            printf("],\"energy_Wh\":[");
            for (int ch = 0; ch < 4; ch++) {
                printf("%.6f%s", get_energy_Wh(ch), ch < 3 ? "," : "");
            }
            printf("],\"log_entries\":%u}\n", static_cast<unsigned>(log_entries_count()));
        }

        delay(1000);
    }

    return 0;
}
