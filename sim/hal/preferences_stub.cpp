#include "Preferences.h"
#include <map>
#include <string>
#include <cstring>

static std::map<std::string, std::string> g_store;

bool Preferences::begin(const char* name, bool readOnly) {
    (void)name;
    (void)readOnly;
    return true;
}

void Preferences::end() {}

bool Preferences::isKey(const char* key) {
    return g_store.find(key) != g_store.end();
}

size_t Preferences::getBytesLength(const char* key) {
    auto it = g_store.find(key);
    if (it == g_store.end()) return 0;
    return it->second.size();
}

size_t Preferences::getBytes(const char* key, void* buf, size_t maxLen) {
    auto it = g_store.find(key);
    if (it == g_store.end()) return 0;
    size_t n = it->second.size();
    if (n > maxLen) n = maxLen;
    std::memcpy(buf, it->second.data(), n);
    return n;
}

size_t Preferences::putBytes(const char* key, const void* buf, size_t len) {
    g_store[key] = std::string(static_cast<const char*>(buf), len);
    return len;
}

size_t Preferences::getString(const char* key, char* buf, size_t maxLen) {
    auto it = g_store.find(key);
    if (it == g_store.end() || maxLen == 0) {
        if (buf && maxLen > 0) buf[0] = '\0';
        return 0;
    }
    size_t n = it->second.size();
    if (n >= maxLen) n = maxLen - 1;
    std::memcpy(buf, it->second.data(), n);
    buf[n] = '\0';
    return n;
}

size_t Preferences::putString(const char* key, const char* value) {
    g_store[key] = value ? value : "";
    return g_store[key].size();
}

uint16_t Preferences::getUShort(const char* key, uint16_t defaultValue) {
    auto it = g_store.find(key);
    if (it == g_store.end() || it->second.size() != sizeof(uint16_t)) return defaultValue;
    uint16_t v;
    std::memcpy(&v, it->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putUShort(const char* key, uint16_t value) {
    g_store[key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

bool Preferences::getBool(const char* key, bool defaultValue) {
    auto it = g_store.find(key);
    if (it == g_store.end() || it->second.size() != sizeof(bool)) return defaultValue;
    bool v;
    std::memcpy(&v, it->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putBool(const char* key, bool value) {
    g_store[key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

uint8_t Preferences::getUChar(const char* key, uint8_t defaultValue) {
    auto it = g_store.find(key);
    if (it == g_store.end() || it->second.size() != sizeof(uint8_t)) return defaultValue;
    uint8_t v;
    std::memcpy(&v, it->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
    g_store[key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

float Preferences::getFloat(const char* key, float defaultValue) {
    auto it = g_store.find(key);
    if (it == g_store.end() || it->second.size() != sizeof(float)) return defaultValue;
    float v;
    std::memcpy(&v, it->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putFloat(const char* key, float value) {
    g_store[key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

uint32_t Preferences::getUInt(const char* key, uint32_t defaultValue) {
    auto it = g_store.find(key);
    if (it == g_store.end() || it->second.size() != sizeof(uint32_t)) return defaultValue;
    uint32_t v;
    std::memcpy(&v, it->second.data(), sizeof(v));
    return v;
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
    g_store[key] = std::string(reinterpret_cast<const char*>(&value), sizeof(value));
    return sizeof(value);
}

bool Preferences::clear() {
    g_store.clear();
    return true;
}

bool Preferences::remove(const char* key) {
    g_store.erase(std::string(key));
    return true;
}
