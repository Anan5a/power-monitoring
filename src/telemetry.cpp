#include "telemetry.h"
#include "sensor_manager.h"
#include "switch_controller.h"
#include "coulomb_counter.h"
#include "energy_counter.h"
#include "data_logger.h"
#include "settings_manager.h"
#include "connectivity_manager.h"  // get_local_ip_str / get_epoch_time
#include "battery_profile.h"
#include "battery_state.h"
#include "cycle_counter.h"
#include "device_identity.h"
#include "ota_client.h"
#include "config.h"
#include <WiFi.h>
#include <Arduino.h>
#include <time.h>

// === Helpers =================================================================

// === Public API ==============================================================

void telemetry_build(TelemetrySnapshot& out) {
    memset(&out, 0, sizeof(out));

    out.schema_version = TELEMETRY_SCHEMA_VERSION;
    strncpy(out.schema, TELEMETRY_PROFILE_STRING, sizeof(out.schema));
    out.schema[sizeof(out.schema) - 1] = '\0';

    // --- Timestamp: epoch seconds + ms-within-second --------------------------
    time_t epoch = get_epoch_time();
    if (epoch <= 0) epoch = time(nullptr);
    out.ts = (uint32_t)epoch;
    out.ts_ms = (uint16_t)(millis() % 1000);

    // --- Device metadata ------------------------------------------------------
    strncpy(out.device.id, get_device_serial(), sizeof(out.device.id) - 1);
    out.device.id[sizeof(out.device.id) - 1] = '\0';
    strncpy(out.device.fw, TELEMETRY_FW_VERSION, sizeof(out.device.fw));
    out.device.fw[sizeof(out.device.fw) - 1] = '\0';
    out.device.uptime_ms = millis();

    // --- WiFi metadata --------------------------------------------------------
    out.wifi.rssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : (int8_t)0;
    const char* ip = get_local_ip_str();
    if (ip) {
        strncpy(out.wifi.ip, ip, sizeof(out.wifi.ip));
        out.wifi.ip[sizeof(out.wifi.ip) - 1] = '\0';
    }

    // --- Channels -------------------------------------------------------------
    // Walk logical channels through the pod model, mirroring the legacy
    // virtual-channel resolver so V/I/P line up with what the OLED and serial
    // CLI report. Each channel also gets its cumulative energy/charge.
    //
    // We pass a static empty SensorSnapshot& to the get_sensor_* helpers
    // because those overloads ignore the snapshot (they pull from the live
    // pod state). A real snapshot could be queued in here, but the network
    // task already has a fresh one — pass it via telemetry_build_with() if
    // sub-second freshness matters.
    static SensorSnapshot s_empty_snapshot;  // zero-init; ignored by helpers
    uint8_t lcount = sensor_get_logical_channel_count();
    if (lcount > TELEMETRY_MAX_CHANNELS) lcount = TELEMETRY_MAX_CHANNELS;
    out.channel_count = lcount;

    for (uint8_t ch = 0; ch < lcount; ch++) {
        TelemetryChannel& tc = out.channels[ch];
        tc.ch = ch;

        // Virtual channel resolution: V/I from the configured sources, with
        // sensible fallbacks (logical channel). Power = V * I unless the
        // current source is INA226 (it has built-in power).
        VirtualChannelConfig vc;
        float v = 0.0f, i = 0.0f, p = 0.0f;
        if (settings_load_virtual_channel(ch, &vc) &&
            (vc.voltage_src > 0 || vc.current_src > 0)) {
            if (vc.voltage_src > 0) {
                v = get_sensor_voltage(vc.voltage_src, vc.voltage_idx, s_empty_snapshot);
            }
            if (vc.current_src > 0) {
                i = get_sensor_current(vc.current_src, vc.current_idx, s_empty_snapshot);
                if (vc.current_src == 3) {
                    p = get_sensor_power(vc.current_src, vc.current_idx, s_empty_snapshot);
                } else if (vc.voltage_src > 0) {
                    p = v * i;
                }
            }
        } else {
            v = get_channel_voltage(ch);
            i = get_channel_current(ch);
            p = (ch == 3) ? get_channel_power(ch) : (v * i);
        }
        tc.V = v;
        tc.I = i;
        tc.P = p;
        tc.energy_Wh  = get_energy_Wh(ch);
        tc.charge_mAh = get_coulomb_mAh(ch);
    }

    // --- Switches -------------------------------------------------------------
    // We do not depend on the sensor task snapshot here — pulls are cheap
    // (NVS + GPIO read). Use settings_load_switch_count() as the source of
    // truth for switch count, capped at MAX_SWITCHES.
    uint8_t sw_count = settings_load_switch_count();
    if (sw_count > TELEMETRY_MAX_SWITCHES) sw_count = TELEMETRY_MAX_SWITCHES;
    out.switch_count = sw_count;

    for (uint8_t i = 0; i < sw_count; i++) {
        TelemetrySwitch& ts = out.switches[i];
        SwitchChannel ch;
        SwitchRule rule;
        if (!settings_load_switch(i, &ch)) {
            ts.idx = i;
            ts.type = 0;
            ts.state = false;
            ts.auto_mode = false;
            ts.rule_tripped = false;
            continue;
        }
        ts.idx = i;
        ts.type = ch.type;
        ts.state = get_switch_state(i);
        ts.auto_mode = switch_get_auto_enabled();
        // rule_tripped: LATCHED state, not a live evaluation. True iff
        // the relay is currently energized AND its rule is enabled. This
        // is the wire-format-compatible "has the rule fired and stuck?"
        // signal; it is NOT "are the rule's combined conditions satisfied
        // right now?". Consumers that need the latter must evaluate the
        // rule themselves against the channels[] V/I/P in this snapshot.
        ts.rule_tripped = ts.state && settings_load_switch_rule(i, &rule) && rule.enabled;
    }

    // --- Batteries ------------------------------------------------------------
    // One entry per logical channel that has a battery profile BINDING
    // (battery_channel_profile(ch) != BATTERY_CHANNEL_NO_BINDING). Channels
    // with no binding are skipped — no row is emitted. The channel → row
    // mapping is deterministic and stable across publishes (lower channel
    // index → earlier row), so consumers can rely on positional ordering.
    //
    // All numeric values come from the canonical owners:
    //   - cycle_counter : equivalent_full_cycles, cumulative_Ah_in,
    //                     cumulative_Ah_out, soc_pct, V, I, last_update_ms
    //   - capacity_test : capacity_test_active, capacity_test_soh_pct
    // No local SoC / cycle / Ah math lives here; this is a read-only view
    // of the per-channel BatteryState maintained by the sensor task at 1Hz.
    uint8_t bat_count = 0;
    for (uint8_t ch = 0; ch < lcount && bat_count < TELEMETRY_MAX_BATTERIES; ch++) {
        // Filter: must have a profile binding. (This is the new "is this a
        // battery channel?" check; the legacy settings_load_battery* probe
        // is no longer the source of truth for the JSON battery[] array.)
        if (battery_channel_profile(ch) == BATTERY_CHANNEL_NO_BINDING) continue;
        if (!cycle_counter_is_active(ch)) continue;

        uint8_t pid = battery_channel_profile(ch);
        const BatteryChemistryProfile* bp = battery_profile_get(pid);

        // Point-in-time snapshot of the channel state under a single
        // critical-section lock. The previous implementation called seven
        // getters sequentially, each taking its own lock — a sensorTask
        // tick in between could have updated cumulative_Ah_in but not yet
        // last_V, so the published snapshot could mix frames. The snapshot
        // helper eliminates that race.
        BatteryState st;
        cycle_counter_snapshot(ch, &st);

        TelemetryBattery& tb = out.battery[bat_count++];
        tb.ch = ch;
        tb.profile_id = pid;
        tb.chemistry = bp ? bp->chemistry : 0;
        tb.rated_Ah  = bp ? bp->rated_capacity_Ah : 0.0f;

        // Authoritative values from the snapshot (same fields as before;
        // ordering under one lock guarantees they're a consistent view).
        tb.soc_pct             = st.last_SoC_pct;
        tb.V                   = st.last_V;
        tb.I                   = st.last_I;
        tb.cumulative_Ah_in    = st.cumulative_Ah_in;
        tb.cumulative_Ah_out   = st.cumulative_Ah_out;
        tb.equivalent_full_cycles = st.equivalent_full_cycles;
        (void)cycle_counter_get_last_update_ms;  // not exposed in TelemetryBattery
        tb.soh_pct     = st.soh_pct;
        tb.soh_samples = st.soh_samples;
    }
    out.battery_count = bat_count;

    // --- Log metadata ---------------------------------------------------------
    uint32_t entries = log_entries_count();
    out.log.entries  = (entries > 0xFFFF) ? 0xFFFF : (uint16_t)entries;
    out.log.overflow = log_has_overflow_file();

    // --- Heap -----------------------------------------------------------------
    out.heap_free = ESP.getFreeHeap();

    // --- OTA status -------------------------------------------------------------
    OtaState ota_st = ota_get_state();
    out.ota.ota_in_progress = (ota_st == OTA_DOWNLOADING || ota_st == OTA_APPLYING);
    const char* ver = ota_get_version();
    if (ver) {
        strncpy(out.ota.ota_version, ver, sizeof(out.ota.ota_version));
        out.ota.ota_version[sizeof(out.ota.ota_version) - 1] = '\0';
    }
    out.ota.ota_progress_pct = ota_get_progress_pct();
    const char* status_str = "";
    switch (ota_st) {
        case OTA_IDLE:        status_str = "idle";        break;
        case OTA_CHECKING:    status_str = "checking";    break;
        case OTA_DOWNLOADING: status_str = "downloading"; break;
        case OTA_APPLYING:    status_str = "applying";    break;
        case OTA_REBOOTING:   status_str = "rebooting";   break;
        case OTA_FAILED:      status_str = "failed";      break;
    }
    strncpy(out.ota.ota_status, status_str, sizeof(out.ota.ota_status));
    out.ota.ota_status[sizeof(out.ota.ota_status) - 1] = '\0';
}

