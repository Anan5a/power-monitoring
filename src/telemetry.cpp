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
#include "capacity_test.h"
#include "config.h"
#include <WiFi.h>
#include <Arduino.h>
#include <time.h>

// === Schema constants ========================================================

// Capacity-test field: the most recent SoH reading published after a capacity
// test completes. We hold it for a single publish so a watching client can
// observe SoH without needing to poll. 0 = no test just completed.
//
// This is a one-shot side channel driven by capacity_test.cpp; once consumed
// by telemetry_build() the flag is cleared. capacity_test_last_soh_pct() is
// preferred for new readers, but this hold-window is preserved for back-compat
// with the TelemetryBattery.capacity_test_soh_valid field.
static float     g_last_capacity_test_soh = 0.0f;
static bool      g_last_capacity_test_soh_valid = false;
static uint32_t  g_last_capacity_test_ms = 0;
#define CAPACITY_TEST_SOH_HOLD_MS 30000UL

// === Helpers =================================================================

static void format_mac_device_id(char* out, size_t out_len) {
    // MAC string is 17 chars ("AA:BB:CC:DD:EE:FF"). Strip colons to fit 12 hex
    // chars in 24-byte buffer; falls back to "unknown" if WiFi MAC isn't ready.
    uint8_t mac[6] = {0};
    if (WiFi.macAddress(mac) != 0 || (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0) {
        snprintf(out, out_len, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strncpy(out, "unknown", out_len);
        out[out_len - 1] = '\0';
    }
}

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
    format_mac_device_id(out.device.id, sizeof(out.device.id));
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
        // rule_tripped is "is the combined condition currently satisfied?"
        // We treat it conservatively: a switch is "tripped" if its rule is
        // enabled AND it is energized (i.e. the rule fired and we're in the
        // active state). A future enhancement could re-evaluate the rule
        // against the latest snapshot here.
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

        TelemetryBattery& tb = out.battery[bat_count++];
        tb.ch = ch;
        tb.profile_id = pid;
        tb.chemistry = bp ? bp->chemistry : 0;
        tb.rated_Ah  = bp ? bp->rated_capacity_Ah : 0.0f;

        // Authoritative values from cycle_counter. These are updated every
        // 1Hz tick in sensorTask; publish (5s cadence) always reads at least
        // one tick old.
        tb.soc_pct             = cycle_counter_get_last_soc_pct(ch);
        tb.V                   = cycle_counter_get_last_voltage(ch);
        tb.I                   = cycle_counter_get_last_current(ch);
        tb.cumulative_Ah_in    = cycle_counter_get_cumulative_Ah_in(ch);
        tb.cumulative_Ah_out   = cycle_counter_get_cumulative_Ah_out(ch);
        tb.equivalent_full_cycles = cycle_counter_get_equivalent_full_cycles(ch);
        (void)cycle_counter_get_last_update_ms;  // not exposed in TelemetryBattery
        tb.capacity_test_active  = capacity_test_is_active(ch);
        tb.capacity_test_soh_pct = capacity_test_last_soh_pct(ch);
        // capacity_test_soh_valid stays true if either the cycle_counter's
        // pending result or the one-shot side channel set a recent value.
        // The one-shot hold window is preserved for back-compat: callers that
        // just want "did a test finish recently?" still see the legacy flag.
        bool soh_pending = (tb.capacity_test_soh_pct >= 0.0f) ||
            (g_last_capacity_test_soh_valid &&
             (millis() - g_last_capacity_test_ms) < CAPACITY_TEST_SOH_HOLD_MS);
        tb.capacity_test_soh_valid = soh_pending;
        if (g_last_capacity_test_soh_valid &&
            (millis() - g_last_capacity_test_ms) < CAPACITY_TEST_SOH_HOLD_MS) {
            // One-shot: clear after the holding window so it only shows up
            // in a single publish.
            g_last_capacity_test_soh_valid = false;
            g_last_capacity_test_soh = 0.0f;
        }
    }
    out.battery_count = bat_count;

    // --- Log metadata ---------------------------------------------------------
    uint32_t entries = log_entries_count();
    out.log.entries  = (entries > 0xFFFF) ? 0xFFFF : (uint16_t)entries;
    out.log.overflow = log_has_overflow_file();

    // --- Heap -----------------------------------------------------------------
    out.heap_free = ESP.getFreeHeap();
}

// === Capacity-test side channel ==============================================
// Called from the capacity test driver (or any test controller) when a test
// completes. Surfaces the SoH value to the next telemetry publish and
// expires it after the hold window so consumers don't have to.
void telemetry_publish_capacity_test_soh(float soh_pct) {
    g_last_capacity_test_soh = soh_pct;
    g_last_capacity_test_soh_valid = true;
    g_last_capacity_test_ms = millis();
}
