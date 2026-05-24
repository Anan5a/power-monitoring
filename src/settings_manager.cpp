#include "settings_manager.h"
#include <Preferences.h>

static Preferences prefs;

void init_settings() {
    prefs.begin("pm-settings", false);
}

bool settings_load_wifi(char* ssid, char* pass, size_t buf_len) {
    if (!prefs.isKey("wifi_ssid")) return false;
    strlcpy(ssid, prefs.getString("wifi_ssid", "").c_str(), buf_len);
    strlcpy(pass, prefs.getString("wifi_pass", "").c_str(), buf_len);
    return true;
}
void settings_save_wifi(const char* ssid, const char* pass) {
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
}

bool settings_load_mqtt(char* broker, uint16_t* port, char* topic, size_t buf_len) {
    if (!prefs.isKey("mqtt_broker")) return false;
    strlcpy(broker, prefs.getString("mqtt_broker", "").c_str(), buf_len);
    *port = prefs.getUShort("mqtt_port", 1883);
    strlcpy(topic, prefs.getString("mqtt_topic", "").c_str(), buf_len);
    return true;
}
void settings_save_mqtt(const char* broker, uint16_t port, const char* topic) {
    prefs.putString("mqtt_broker", broker);
    prefs.putUShort("mqtt_port", port);
    prefs.putString("mqtt_topic", topic);
}

bool settings_load_http_endpoint(char* url, char* auth_token, size_t buf_len) {
    if (!prefs.isKey("http_url")) return false;
    strlcpy(url, prefs.getString("http_url", "").c_str(), buf_len);
    strlcpy(auth_token, prefs.getString("http_token", "").c_str(), buf_len);
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
    strlcpy(url, prefs.getString("supa_url", "").c_str(), buf_len);
    return true;
}
void settings_save_supabase_url(const char* url) {
    prefs.putString("supa_url", url);
}
bool settings_load_supabase_anon_key(char* key, size_t buf_len) {
    if (!prefs.isKey("supa_anon")) return false;
    strlcpy(key, prefs.getString("supa_anon", "").c_str(), buf_len);
    return true;
}
void settings_save_supabase_anon_key(const char* key) {
    prefs.putString("supa_anon", key);
}
bool settings_load_supabase_api_key(char* key, size_t buf_len) {
    if (!prefs.isKey("supa_api")) return false;
    strlcpy(key, prefs.getString("supa_api", "").c_str(), buf_len);
    return true;
}
void settings_save_supabase_api_key(const char* key) {
    prefs.putString("supa_api", key);
}
bool settings_load_supabase_device_key(char* key, size_t buf_len) {
    if (!prefs.isKey("supa_dev")) return false;
    strlcpy(key, prefs.getString("supa_dev", "").c_str(), buf_len);
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
    if (len != sizeof(RelayRule)) return false;
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

bool settings_load_calibration(Calibration* out) {
    if (!prefs.isKey("cal")) return false;
    prefs.getBytes("cal", out, sizeof(Calibration));
    return true;
}
void settings_save_calibration(const Calibration* in) {
    prefs.putBytes("cal", in, sizeof(Calibration));
}

bool settings_load_channel_calibration(ChannelCalibration* out) {
    if (!prefs.isKey("chan_cal")) return false;
    size_t len = prefs.getBytes("chan_cal", out, sizeof(ChannelCalibration));
    return len == sizeof(ChannelCalibration);
}
void settings_save_channel_calibration(const ChannelCalibration* in) {
    prefs.putBytes("chan_cal", in, sizeof(ChannelCalibration));
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
    strlcpy(out, prefs.getString(key, "").c_str(), buf_len);
    return true;
}
void settings_save_channel_name(uint8_t channel, const char* name) {
    char key[16];
    snprintf(key, sizeof(key), "chan_name_%d", channel);
    prefs.putString(key, name);
}

uint32_t settings_load_ble_pin() {
    return prefs.getUInt("ble_pin", 123456);
}
void settings_save_ble_pin(uint32_t pin) {
    prefs.putUInt("ble_pin", pin);
}

void settings_factory_reset() {
    prefs.clear();
}
