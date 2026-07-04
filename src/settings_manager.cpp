#include "settings_manager.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void init_settings() {
    prefs.begin("pm-settings", false);
}

bool settings_load_wifi(char* ssid, char* pass, size_t buf_len) {
    if (!prefs.isKey("wifi_ssid")) return false;
    prefs.getString("wifi_ssid", ssid, buf_len);
    prefs.getString("wifi_pass", pass, buf_len);
    return true;
}
void settings_save_wifi(const char* ssid, const char* pass) {
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
}

bool settings_load_mqtt(char* broker, uint16_t* port, char* topic, size_t buf_len) {
    if (!prefs.isKey("mqtt_broker")) return false;
    prefs.getString("mqtt_broker", broker, buf_len);
    *port = prefs.getUShort("mqtt_port", 1883);
    prefs.getString("mqtt_topic", topic, buf_len);
    return true;
}
void settings_save_mqtt(const char* broker, uint16_t port, const char* topic) {
    prefs.putString("mqtt_broker", broker);
    prefs.putUShort("mqtt_port", port);
    prefs.putString("mqtt_topic", topic);
}

bool settings_load_http_endpoint(char* url, char* auth_token, size_t buf_len) {
    if (!prefs.isKey("http_url")) return false;
    prefs.getString("http_url", url, buf_len);
    prefs.getString("http_token", auth_token, buf_len);
    return true;
}
void settings_save_http_endpoint(const char* url, const char* auth_token) {
    prefs.putString("http_url", url);
    prefs.putString("http_token", auth_token);
}
bool settings_load_http_enabled() {
    return prefs.getBool("http_en", false);
}
void settings_save_http_enabled(bool enabled) {
    prefs.putBool("http_en", enabled);
}

bool settings_load_supabase_url(char* url, size_t buf_len) {
    if (!prefs.isKey("supa_url")) return false;
    prefs.getString("supa_url", url, buf_len);
    return true;
}
void settings_save_supabase_url(const char* url) {
    prefs.putString("supa_url", url);
}
bool settings_load_supabase_anon_key(char* key, size_t buf_len) {
    if (!prefs.isKey("supa_anon")) return false;
    prefs.getString("supa_anon", key, buf_len);
    return true;
}
void settings_save_supabase_anon_key(const char* key) {
    prefs.putString("supa_anon", key);
}
bool settings_load_supabase_api_key(char* key, size_t buf_len) {
    if (!prefs.isKey("supa_api")) return false;
    prefs.getString("supa_api", key, buf_len);
    return true;
}
void settings_save_supabase_api_key(const char* key) {
    prefs.putString("supa_api", key);
}
bool settings_load_supabase_device_key(char* key, size_t buf_len) {
    if (!prefs.isKey("supa_dev")) return false;
    prefs.getString("supa_dev", key, buf_len);
    return true;
}
void settings_save_supabase_device_key(const char* key) {
    prefs.putString("supa_dev", key);
}

uint8_t settings_load_relay_count() {
    return prefs.getUChar("relay_count", 0);
}
bool settings_load_relay(uint8_t idx, RelayRule* out) {
    char key[16];
    snprintf(key, sizeof(key), "relay_%d", idx);
    if (!prefs.isKey(key)) return false;
    size_t len = prefs.getBytesLength(key);
    if (len != sizeof(RelayRule)) {
        // Backward compat: older struct without is_energized field — zero new fields
        if (len < sizeof(RelayRule)) {
            memset(out, 0, sizeof(RelayRule));
            prefs.getBytes(key, out, len);
            out->is_energized = false;  // default to off for migrated entries
            return true;
        }
        return false;
    }
    prefs.getBytes(key, out, sizeof(RelayRule));
    return true;
}
void settings_save_relay(uint8_t idx, const RelayRule* in) {
    char key[16];
    snprintf(key, sizeof(key), "relay_%d", idx);
    prefs.putBytes(key, in, sizeof(RelayRule));
    uint8_t count = settings_load_relay_count();
    if (idx >= count) prefs.putUChar("relay_count", idx + 1);
}

uint8_t settings_load_switch_count() {
    return prefs.getUChar("switch_count", 0);
}
bool settings_load_switch(uint8_t idx, SwitchChannel* out) {
    char key[24];
    snprintf(key, sizeof(key), "sw_ch_%d", idx);
    if (!prefs.isKey(key)) return false;
    size_t len = prefs.getBytesLength(key);
    if (len != sizeof(SwitchChannel)) {
        if (len < sizeof(SwitchChannel)) {
            memset(out, 0, sizeof(SwitchChannel));
            prefs.getBytes(key, out, len);
            return true;
        }
        return false;
    }
    prefs.getBytes(key, out, sizeof(SwitchChannel));
    return true;
}
void settings_save_switch(uint8_t idx, const SwitchChannel* in) {
    char key[24];
    snprintf(key, sizeof(key), "sw_ch_%d", idx);
    prefs.putBytes(key, in, sizeof(SwitchChannel));
    uint8_t count = settings_load_switch_count();
    if (idx >= count) prefs.putUChar("switch_count", idx + 1);
}
bool settings_load_switch_rule(uint8_t idx, SwitchRule* out) {
    char key[24];
    snprintf(key, sizeof(key), "sw_rule_%d", idx);
    if (!prefs.isKey(key)) return false;
    size_t len = prefs.getBytesLength(key);
    if (len != sizeof(SwitchRule)) {
        if (len < sizeof(SwitchRule)) {
            memset(out, 0, sizeof(SwitchRule));
            prefs.getBytes(key, out, len);
            return true;
        }
        return false;
    }
    prefs.getBytes(key, out, sizeof(SwitchRule));
    return true;
}
void settings_save_switch_rule(uint8_t idx, const SwitchRule* in) {
    char key[24];
    snprintf(key, sizeof(key), "sw_rule_%d", idx);
    prefs.putBytes(key, in, sizeof(SwitchRule));
}

bool settings_load_calibration(Calibration* out) {
    if (!prefs.isKey("cal")) return false;
    prefs.getBytes("cal", out, sizeof(Calibration));
    return true;
}
void settings_save_calibration(const Calibration* in) {
    prefs.putBytes("cal", in, sizeof(Calibration));
}

// Version byte for the ChannelCalibration blob. Bump when the struct layout
// (i.e. array sizes) changes. The legacy 48-byte blob (3-element arrays
// without invert_curr) is version 1; the MAX_LOGICAL_CHANNELS-sized blob
// is version 2.
static const uint8_t kCalBlobVersion = 2;
static const char kCalBlobVersionKey[] = "chan_cal_ver";
// Size of the legacy v1 blob (3 channels, no invert_curr).
//   4 float[3] arrays = 12 * 4 = 48 bytes
static const size_t kCalBlobV1Size = 48;

bool settings_load_channel_calibration(ChannelCalibration* out) {
    if (!prefs.isKey("chan_cal")) return false;
    size_t len = prefs.getBytes("chan_cal", out, sizeof(ChannelCalibration));
    // Reject blobs larger than the current struct — never happens in
    // production but guards against accidentally growing MAX_LOGICAL_CHANNELS.
    if (len == sizeof(ChannelCalibration)) {
        // Version 2 — current layout. Make sure the version byte matches;
        // if not, a future field reshuffled the layout and we should drop
        // the blob rather than mis-decode it.
        uint8_t stored = prefs.getUChar(kCalBlobVersionKey, 0);
        if (stored != kCalBlobVersion) {
            return false;
        }
        return true;
    }
    // Backward compat: legacy v1 blob (48 bytes, 3-element arrays, no invert_curr).
    if (len == kCalBlobV1Size) {
        memset(out, 0, sizeof(ChannelCalibration));
        prefs.getBytes("chan_cal", out, len);
        // Stamp the version byte so subsequent loads treat it as upgraded.
        prefs.putUChar(kCalBlobVersionKey, kCalBlobVersion);
        return true;
    }
    return false;
}
void settings_save_channel_calibration(const ChannelCalibration* in) {
    prefs.putBytes("chan_cal", in, sizeof(ChannelCalibration));
    prefs.putUChar(kCalBlobVersionKey, kCalBlobVersion);
}

float settings_load_coulomb_mAh(uint8_t channel) {
    char key[16];
    snprintf(key, sizeof(key), "coul_%d", channel);
    return prefs.getFloat(key, 0.0f);
}
void settings_save_coulomb_mAh(uint8_t channel, float mAh) {
    char key[16];
    snprintf(key, sizeof(key), "coul_%d", channel);
    prefs.putFloat(key, mAh);
}

float settings_load_energy_Wh(uint8_t channel) {
    char key[16];
    snprintf(key, sizeof(key), "enwh_%d", channel);
    return prefs.getFloat(key, 0.0f);
}
void settings_save_energy_Wh(uint8_t channel, float wh) {
    char key[16];
    snprintf(key, sizeof(key), "enwh_%d", channel);
    prefs.putFloat(key, wh);
}

bool settings_load_battery(uint8_t channel, BatteryConfig* out) {
    char key[16];
    snprintf(key, sizeof(key), "bat_%d", channel);
    if (!prefs.isKey(key)) return false;
    if (prefs.getBytesLength(key) != sizeof(BatteryConfig)) return false;
    prefs.getBytes(key, out, sizeof(BatteryConfig));
    return true;
}
void settings_save_battery(uint8_t channel, const BatteryConfig* in) {
    char key[16];
    snprintf(key, sizeof(key), "bat_%d", channel);
    prefs.putBytes(key, in, sizeof(BatteryConfig));
}

bool settings_load_battery_profile(uint8_t channel, BatteryProfile* out) {
    char key[16];
    snprintf(key, sizeof(key), "bat_prof_%d", channel);
    if (!prefs.isKey(key)) return false;
    if (prefs.getBytesLength(key) != sizeof(BatteryProfile)) return false;
    prefs.getBytes(key, out, sizeof(BatteryProfile));
    return true;
}
void settings_save_battery_profile(uint8_t channel, const BatteryProfile* in) {
    char key[16];
    snprintf(key, sizeof(key), "bat_prof_%d", channel);
    prefs.putBytes(key, in, sizeof(BatteryProfile));
}

uint8_t settings_load_channel_group_count() {
    return prefs.getUChar("chan_grp_count", 0);
}
bool settings_load_channel_group(uint8_t idx, ChannelGroup* out) {
    char key[16];
    snprintf(key, sizeof(key), "chan_grp_%d", idx);
    if (!prefs.isKey(key)) return false;
    if (prefs.getBytesLength(key) != sizeof(ChannelGroup)) return false;
    prefs.getBytes(key, out, sizeof(ChannelGroup));
    return true;
}
void settings_save_channel_group(uint8_t idx, const ChannelGroup* in) {
    char key[16];
    snprintf(key, sizeof(key), "chan_grp_%d", idx);
    prefs.putBytes(key, in, sizeof(ChannelGroup));
    uint8_t count = settings_load_channel_group_count();
    if (idx >= count) prefs.putUChar("chan_grp_count", idx + 1);
}

bool settings_load_channel_name(uint8_t channel, char* out, size_t buf_len) {
    char key[16];
    snprintf(key, sizeof(key), "chan_name_%d", channel);
    if (!prefs.isKey(key)) return false;
    prefs.getString(key, out, buf_len);
    return true;
}
void settings_save_channel_name(uint8_t channel, const char* name) {
    char key[16];
    snprintf(key, sizeof(key), "chan_name_%d", channel);
    prefs.putString(key, name);
}

bool settings_load_shunt(uint8_t channel, float* out) {
    char key[16];
    snprintf(key, sizeof(key), "shunt_%d", channel);
    if (!prefs.isKey(key)) return false;
    *out = prefs.getFloat(key, 0.0f);
    return *out > 0.0f;
}
void settings_save_shunt(uint8_t channel, float ohms) {
    char key[16];
    snprintf(key, sizeof(key), "shunt_%d", channel);
    prefs.putFloat(key, ohms);
}

bool settings_load_volt_ratio(uint8_t channel, float* out) {
    char key[16];
    snprintf(key, sizeof(key), "volt_ratio_%d", channel);
    if (!prefs.isKey(key)) return false;
    *out = prefs.getFloat(key, 0.0f);
    return *out > 0.0f;
}
void settings_save_volt_ratio(uint8_t channel, float ratio) {
    char key[16];
    snprintf(key, sizeof(key), "volt_ratio_%d", channel);
    prefs.putFloat(key, ratio);
}

bool settings_load_resistors(uint8_t channel, float* r_high, float* r_low) {
    char key_h[16], key_l[16];
    snprintf(key_h, sizeof(key_h), "r_high_%d", channel);
    snprintf(key_l, sizeof(key_l), "r_low_%d", channel);
    if (!prefs.isKey(key_h) || !prefs.isKey(key_l)) return false;
    *r_high = prefs.getFloat(key_h, 0.0f);
    *r_low = prefs.getFloat(key_l, 1.0f);
    return *r_high > 0.0f && *r_low > 0.0f;
}
void settings_save_resistors(uint8_t channel, float r_high, float r_low) {
    char key_h[16], key_l[16];
    snprintf(key_h, sizeof(key_h), "r_high_%d", channel);
    snprintf(key_l, sizeof(key_l), "r_low_%d", channel);
    prefs.putFloat(key_h, r_high);
    prefs.putFloat(key_l, r_low);
}

uint32_t settings_load_ble_pin() {
    return prefs.getUInt("ble_pin", 123456);
}
void settings_save_ble_pin(uint32_t pin) {
    prefs.putUInt("ble_pin", pin);
}

bool settings_load_virtual_channel(uint8_t ch, VirtualChannelConfig* out) {
    char key[16];
    snprintf(key, sizeof(key), "vc_%d", ch);
    if (!prefs.isKey(key)) return false;
    size_t len = prefs.getBytesLength(key);
    if (len != sizeof(VirtualChannelConfig)) return false;
    prefs.getBytes(key, out, sizeof(VirtualChannelConfig));
    return true;
}
void settings_save_virtual_channel(uint8_t ch, const VirtualChannelConfig* in) {
    char key[16];
    snprintf(key, sizeof(key), "vc_%d", ch);
    prefs.putBytes(key, in, sizeof(VirtualChannelConfig));
}

// ── Auto-discovered sensor config ─────────────────────────────────────────────

uint8_t settings_load_discovered_ina_count() {
    return prefs.getUChar("disc_ina_cnt", 0);
}
void settings_save_discovered_ina_count(uint8_t count) {
    prefs.putUChar("disc_ina_cnt", count);
}
bool settings_load_discovered_ina_addr(uint8_t idx, uint8_t* addr) {
    char key[16];
    snprintf(key, sizeof(key), "dina_a_%d", idx);
    if (!prefs.isKey(key)) return false;
    *addr = prefs.getUChar(key, 0);
    return true;
}
void settings_save_discovered_ina_addr(uint8_t idx, uint8_t addr) {
    char key[16];
    snprintf(key, sizeof(key), "dina_a_%d", idx);
    prefs.putUChar(key, addr);
}
bool settings_load_discovered_ina_shunt(uint8_t idx, float* shunt) {
    char key[16];
    snprintf(key, sizeof(key), "dina_sh_%d", idx);
    if (!prefs.isKey(key)) return false;
    *shunt = prefs.getFloat(key, 0.005f);
    return true;
}
void settings_save_discovered_ina_shunt(uint8_t idx, float shunt) {
    char key[16];
    snprintf(key, sizeof(key), "dina_sh_%d", idx);
    prefs.putFloat(key, shunt);
}
bool settings_load_discovered_ina_vratio(uint8_t idx, float* ratio) {
    char key[16];
    snprintf(key, sizeof(key), "dina_vr_%d", idx);
    if (!prefs.isKey(key)) return false;
    *ratio = prefs.getFloat(key, 1.0f);
    return true;
}
void settings_save_discovered_ina_vratio(uint8_t idx, float ratio) {
    char key[16];
    snprintf(key, sizeof(key), "dina_vr_%d", idx);
    prefs.putFloat(key, ratio);
}
uint8_t settings_load_discovered_bl_count() {
    return prefs.getUChar("disc_bl_cnt", 0);
}
void settings_save_discovered_bl_count(uint8_t count) {
    prefs.putUChar("disc_bl_cnt", count);
}
bool settings_load_discovered_bl_addr(uint8_t idx, uint8_t* addr) {
    char key[16];
    snprintf(key, sizeof(key), "dbl_a_%d", idx);
    if (!prefs.isKey(key)) return false;
    *addr = prefs.getUChar(key, 0);
    return true;
}
void settings_save_discovered_bl_addr(uint8_t idx, uint8_t addr) {
    char key[16];
    snprintf(key, sizeof(key), "dbl_a_%d", idx);
    prefs.putUChar(key, addr);
}
void settings_clear_discovered() {
    prefs.remove("disc_ina_cnt");
    prefs.remove("disc_bl_cnt");
    for (uint8_t i = 0; i < MAX_INA226; i++) {
        char key[16];
        snprintf(key, sizeof(key), "dina_a_%d", i); prefs.remove(key);
        snprintf(key, sizeof(key), "dina_sh_%d", i); prefs.remove(key);
        snprintf(key, sizeof(key), "dina_vr_%d", i); prefs.remove(key);
    }
    for (uint8_t i = 0; i < MAX_BL0939; i++) {
        char key[16];
        snprintf(key, sizeof(key), "dbl_a_%d", i); prefs.remove(key);
    }
}

void settings_factory_reset() {
    // Wipe pm-settings (this is the only Preferences instance we own
    // directly — the static `prefs` in this translation unit). Battery
    // profiles and bindings live in the "pm-battery" namespace managed by
    // battery_profile.cpp / battery_state.cpp. Open a fresh scoped handle
    // and clear it here so factory reset covers both namespaces.
    prefs.clear();
    {
        Preferences bat;
        if (bat.begin("pm-battery", false)) {
            bat.clear();
            bat.end();
        }
    }
}
