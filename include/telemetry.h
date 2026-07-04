#ifndef TELEMETRY_H
#define TELEMETRY_H

// telemetry.h
// =============================================================================
// Single full-snapshot publish payload shared by MQTT, Supabase, HTTP custom
// endpoint, and BLE sensor notify.  All transports serialize the same struct
// so web/Supabase clients see the same shape regardless of transport.
//
// Schema version: TELEMETRY_SCHEMA_VERSION. Bump whenever a field is added,
// renamed, removed, or its unit/semantic changes. The server uses this to
// branch parser logic.
//
// "telemetry_v1" Content-Profile on Supabase and the schema "v" field in
// JSON both reference this version. MQTT_LEGACY_PAYLOAD=1 keeps the old
// flat-channel payload for backward compatibility with existing consumers.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

// Upper bounds for fixed-size arrays. Saturation = 16 channels + 8 batteries
// + 8 switches. Sized for headroom against the current 4-channel build.
#define TELEMETRY_MAX_CHANNELS  16
#define TELEMETRY_MAX_SWITCHES  8
#define TELEMETRY_MAX_BATTERIES 8

#define TELEMETRY_SCHEMA_VERSION 1
#define TELEMETRY_PROFILE_STRING "telemetry_v1"
#define TELEMETRY_FW_VERSION     "2.0.0"

struct TelemetryDevice {
    char     id[24];       // MAC-derived device id (e.g. "AABBCCDDEEFF")
    char     fw[16];       // firmware version
    uint32_t uptime_ms;    // millis() at sample time
};

struct TelemetryWifi {
    int8_t rssi;           // WiFi.RSSI() in dBm
    char   ip[16];         // "x.x.x.x" (IPv4 string)
};

struct TelemetryChannel {
    uint8_t ch;            // logical channel index
    float   V;             // volts
    float   I;             // amps (signed: negative = reverse)
    float   P;             // watts (= V * I, or sensor-reported for INA226)
    float   energy_Wh;     // cumulative from energy_counter
    float   charge_mAh;    // cumulative from coulomb_counter
};

struct TelemetrySwitch {
    uint8_t idx;           // 0..MAX_SWITCHES-1
    uint8_t type;          // SwitchType (RELAY, MOSFET, SSR, ...)
    bool    state;         // energized (logical state, not GPIO level)
    bool    auto_mode;     // auto-trip evaluation enabled
    bool    rule_tripped;  // combined condition currently active
};

struct TelemetryBattery {
    uint8_t ch;                  // logical channel index
    uint8_t profile_id;          // 0..N — NVS profile slot
    uint8_t chemistry;           // BatteryChemistry enum
    float   rated_Ah;            // profile.capacity_mAh / 1000
    float   soc_pct;             // 0..100
    float   V;                   // current channel voltage
    float   I;                   // current channel current
    float   cumulative_Ah_in;    // positive coulomb accumulated
    float   cumulative_Ah_out;   // |negative| coulomb accumulated
    float   equivalent_full_cycles; // (Ah_in + Ah_out) / (2 * rated_Ah)
    bool    capacity_test_active;
    float   capacity_test_soh_pct;  // only meaningful right after a capacity test
    bool    capacity_test_soh_valid; // true when the above was just reported
};

struct TelemetryLogMeta {
    uint16_t entries;       // log_entries_count() clamped to uint16
    bool     overflow;      // log_has_overflow_file()
};

struct TelemetrySnapshot {
    uint32_t ts;            // unix epoch seconds
    uint16_t ts_ms;         // millis-within-second for sub-second resolution
    uint8_t  schema_version;// mirror of TELEMETRY_SCHEMA_VERSION
    char     schema[16];    // mirror of TELEMETRY_PROFILE_STRING

    TelemetryDevice device;
    TelemetryWifi   wifi;

    uint8_t channel_count;  // number of valid entries in channels[]
    uint8_t switch_count;   // number of valid entries in switches[]
    uint8_t battery_count;  // number of valid entries in battery[]

    TelemetryChannel channels[TELEMETRY_MAX_CHANNELS];
    TelemetrySwitch  switches[TELEMETRY_MAX_SWITCHES];
    TelemetryBattery battery[TELEMETRY_MAX_BATTERIES];

    TelemetryLogMeta log;
    uint32_t heap_free;
};

// Wire the build flag (MQTT_LEGACY_PAYLOAD) into a single source of truth so
// connectivity_manager.cpp can switch payload shape with one #if.
#ifndef MQTT_LEGACY_PAYLOAD
#define MQTT_LEGACY_PAYLOAD 0
#endif

// Build a complete TelemetrySnapshot from current sensor / counter / network
// state. Safe to call from the network task. Pulls from sensor_manager,
// switch_controller, coulomb_counter, energy_counter, data_logger, settings
// (battery profiles / capacity test flags) and the global device metadata.
//
// Cost: a few NVS reads + 16+8+8 small structs. No I/O. Designed to be
// called once per publish (every 5 s on the network task).
void telemetry_build(TelemetrySnapshot& out);

// Capacity-test SoH side channel. Called by the capacity-test driver when a
// test completes. The next telemetry_build() embeds the SoH in the matching
// battery[] entry (capacity_test_soh_valid=true) and the field auto-clears
// after the next publish. Consumers that observe a non-zero SoH can treat
// it as authoritative for that reading only.
void telemetry_publish_capacity_test_soh(float soh_pct);

// Size sanity check. Keep MAX_* honest: anything larger and the 2 KB
// serialized budget is at risk.
static_assert(TELEMETRY_MAX_CHANNELS  >= 16, "raise MAX_CHANNELS or change struct");
static_assert(TELEMETRY_MAX_SWITCHES  >= 8,  "raise MAX_SWITCHES or change struct");
static_assert(TELEMETRY_MAX_BATTERIES >= 8,  "raise MAX_BATTERIES or change struct");
static_assert(sizeof(TelemetrySnapshot) <= 8192,
              "TelemetrySnapshot struct grew past 8 KB — review field sizes");

#endif // TELEMETRY_H
