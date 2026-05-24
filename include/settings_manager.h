#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include <stdint.h>
#include <stddef.h>

enum BatteryChemistry { BAT_LEAD_ACID = 0, BAT_LIPO, BAT_LIION, BAT_NIMH };

struct BatteryConfig {
    uint8_t channel;          // 0-3
    float capacity_mAh;       // total battery capacity for SoC calc
    float initial_soc_pct;    // SoC at last coulomb reset (0-100)
};

struct BatteryProfile {
    uint8_t  channel;          // 0-3
    char     name[24];          // e.g. "12V Lead Acid"
    uint8_t  chemistry;        // BatteryChemistry enum
    float    capacity_mAh;
    float    initial_soc_pct;
    float    cell_count;
    float    full_voltage;     // V per cell * cell_count; 0 = use chemistry default
    float    cutoff_voltage;
    float    float_voltage;
};

struct ChannelGroup {
    uint8_t  group_id;          // 0-3 (max 4 groups)
    char     name[24];          // e.g. "Solar Panel"
    uint8_t  icon;             // 0=solar, 1=battery, 2=load, 3=generic
    uint8_t  channel_mask;    // bitmask: bit 0 = ch0, bit 1 = ch1, etc.
};

struct RelayRule {
    uint8_t channel;          // 0-3
    float overcurrent_A;      // 0 = disabled
    float undervoltage_V;     // 0 = disabled
    float soc_low_pct;        // trip if SoC < this (0 = disabled)
    float soc_high_pct;       // trip if SoC > this (0 = disabled)
    uint16_t trip_delay_ms;   // must exceed this duration to trip
    uint16_t reset_delay_ms;  // must stay below threshold this long to reset
    uint8_t gpio_pin;
    bool active_high;
    bool enabled;
};

struct Calibration {
    float ina3221_v_offset[3];
    float ina3221_i_gain[3];
    float ina226_v_offset;
    float ina226_i_gain;
};

// Per-channel calibration: voltage offset/gain, current offset/gain
struct ChannelCalibration {
    float volt_offset_mv[3];   // mV zero offset per voltage channel
    float volt_gain[3];        // multiplier per voltage channel
    float curr_offset_ma[3];  // mA offset per current channel (ghost current sub)
    float curr_gain[3];        // multiplier per current channel
};

void init_settings();

bool settings_load_wifi(char* ssid, char* pass, size_t buf_len);
void settings_save_wifi(const char* ssid, const char* pass);
bool settings_load_mqtt(char* broker, uint16_t* port, char* topic, size_t buf_len);
void settings_save_mqtt(const char* broker, uint16_t port, const char* topic);

bool settings_load_http_endpoint(char* url, char* auth_token, size_t buf_len);
void settings_save_http_endpoint(const char* url, const char* auth_token);
bool settings_load_http_enabled();
void settings_save_http_enabled(bool enabled);

bool settings_load_supabase_url(char* url, size_t buf_len);
void settings_save_supabase_url(const char* url);
bool settings_load_supabase_anon_key(char* key, size_t buf_len);
void settings_save_supabase_anon_key(const char* key);
bool settings_load_supabase_device_key(char* key, size_t buf_len);
void settings_save_supabase_device_key(const char* key);
bool settings_load_supabase_api_key(char* key, size_t buf_len);
void settings_save_supabase_api_key(const char* key);

uint8_t settings_load_relay_count();
bool settings_load_relay(uint8_t idx, RelayRule* out);
void settings_save_relay(uint8_t idx, const RelayRule* in);

bool settings_load_calibration(Calibration* out);
void settings_save_calibration(const Calibration* in);

bool settings_load_channel_calibration(ChannelCalibration* out);
void settings_save_channel_calibration(const ChannelCalibration* in);

float settings_load_coulomb_mAh(uint8_t channel);
void settings_save_coulomb_mAh(uint8_t channel, float mAh);

bool settings_load_battery(uint8_t channel, BatteryConfig* out);
void settings_save_battery(uint8_t channel, const BatteryConfig* in);

bool settings_load_battery_profile(uint8_t channel, BatteryProfile* out);
void settings_save_battery_profile(uint8_t channel, const BatteryProfile* in);

uint8_t settings_load_channel_group_count();
bool settings_load_channel_group(uint8_t idx, ChannelGroup* out);
void settings_save_channel_group(uint8_t idx, const ChannelGroup* in);

bool settings_load_channel_name(uint8_t channel, char* out, size_t buf_len);
void settings_save_channel_name(uint8_t channel, const char* name);

uint32_t settings_load_ble_pin();        // 6-digit PIN, 0 = no security
void settings_save_ble_pin(uint32_t pin);

void settings_factory_reset();           // wipe all NVS keys

#endif
