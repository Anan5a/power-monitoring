#include "Preferences.h"
#include <map>
#include <string>
#include <cstring>

// Nested map keyed by namespace. The real ESP32 Preferences library
// scopes every key by the namespace passed to begin(); the previous
// single-map stub collided keys across namespaces, breaking any
// settings_manager code that used distinct namespaces for different
// subsystems. The active namespace is recorded in begin() via a static
// (the class itself has no namespace member field) and read by every
// accessor through this file-scope helper.
static std::map<std::string, std::map<std::string, std::string>> g_store;
static const char* kDefaultNs = "pm-settings";
static std::string s_active_ns = kDefaultNs;

static const std::string& active_ns() {
    return s_active_ns;
}

bool Preferences::begin(const char* name, bool readOnly) {
    s_active_ns = name ? std::string(name) : std::string();
    g_store[s_active_ns]; // ensure the namespace bucket exists
    (void)readOnly;
    return true;
}

void Preferences::end() {}

bool Preferences::isKey(const char* key) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return false;
    return it->second.find(key) != it->second.end();
}

size_t Preferences::getBytesLength(const char* key) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return 0;
    auto kv = it->second.find(key);
    if (kv == it->second.end()) return 0;
    return kv->second.size();
}

size_t Preferences::getBytes(const char* key, void* buf, size_t maxLen) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return 0;
    auto kv = it->second.find(key);
    if (kv == it->second.end()) return 0;
    size_t n = kv->second.size();
    if (n > maxLen) n = maxLen;
    std::memcpy(buf, kv->second.data(), n);
    return n;
}

size_t Preferences::putBytes(const char* key, const void* buf, size_t len) {
    const std::string& ns = active_ns();
    g_store[ns][key] = std::string(static_cast<const char*>(buf), len);
    return len;
}

size_t Preferences::getString(const char* key, char* buf, size_t maxLen) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end() || maxLen == 0) {
        if (buf && maxLen > 0) buf[0] = '\0';
        return 0;
    }
    auto kv = it->second.find(key);
    if (kv == it->second.end()) {
        if (buf && maxLen > 0) buf[0] = '\0';
        return 0;
    }
    size_t n = kv->second.size();
    if (n >= maxLen) n = maxLen - 1;
    std::memcpy(buf, kv->second.data(), n);
    buf[n] = '\0';
    return n;
}

size_t Preferences::putString(const char* key, const char* value) {
    const std::string& ns = active_ns();
    g_store[ns][key] = value ? value : "";
    return g_store[ns][key].size();
}

uint16_t Preferences::getUShort(const char* key, uint16_t defaultValue) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return defaultValue;
    auto kv = it->second.find(key);
    if (kv == it->second.end() || kv->second.size() != sizeof(uint16_t)) return defaultValue;
    uint16_t v;
    std::memcpy(&v, kv->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putUShort(const char* key, uint16_t value) {
    const std::string& ns = active_ns();
    g_store[ns][key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

bool Preferences::getBool(const char* key, bool defaultValue) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return defaultValue;
    auto kv = it->second.find(key);
    if (kv == it->second.end() || kv->second.size() != sizeof(bool)) return defaultValue;
    bool v;
    std::memcpy(&v, kv->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putBool(const char* key, bool value) {
    const std::string& ns = active_ns();
    g_store[ns][key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

uint8_t Preferences::getUChar(const char* key, uint8_t defaultValue) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return defaultValue;
    auto kv = it->second.find(key);
    if (kv == it->second.end() || kv->second.size() != sizeof(uint8_t)) return defaultValue;
    uint8_t v;
    std::memcpy(&v, kv->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
    const std::string& ns = active_ns();
    g_store[ns][key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

float Preferences::getFloat(const char* key, float defaultValue) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return defaultValue;
    auto kv = it->second.find(key);
    if (kv == it->second.end() || kv->second.size() != sizeof(float)) return defaultValue;
    float v;
    std::memcpy(&v, kv->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putFloat(const char* key, float value) {
    const std::string& ns = active_ns();
    g_store[ns][key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

uint32_t Preferences::getUInt(const char* key, uint32_t defaultValue) {
    const std::string& ns = active_ns();
    auto it = g_store.find(ns);
    if (it == g_store.end()) return defaultValue;
    auto kv = it->second.find(key);
    if (kv == it->second.end() || kv->second.size() != sizeof(uint32_t)) return defaultValue;
    uint32_t v;
    std::memcpy(&v, kv->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
    const std::string& ns = active_ns();
    g_store[ns][key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

bool Preferences::clear() {
    // Match the real library semantics: clear() only wipes the active
    // namespace, not the entire NVS partition. (Wipe-all is the caller's
    // responsibility in production code too — e.g. iterating every ns.)
    const std::string& ns = active_ns();
    g_store[ns].clear();
    return true;
}

bool Preferences::remove(const char* key) {
    const std::string& ns = active_ns();
    g_store[ns].erase(std::string(key));
    return true;
}
