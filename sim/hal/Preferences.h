#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class Preferences {
public:
    bool begin(const char* name, bool readOnly);
    void end();
    bool isKey(const char* key);

    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* buf, size_t maxLen);
    size_t putBytes(const char* key, const void* buf, size_t len);

    size_t getString(const char* key, char* buf, size_t maxLen);
    size_t putString(const char* key, const char* value);

    uint16_t getUShort(const char* key, uint16_t defaultValue);
    size_t putUShort(const char* key, uint16_t value);

    bool getBool(const char* key, bool defaultValue);
    size_t putBool(const char* key, bool value);

    uint8_t getUChar(const char* key, uint8_t defaultValue);
    size_t putUChar(const char* key, uint8_t value);

    float getFloat(const char* key, float defaultValue);
    size_t putFloat(const char* key, float value);

    uint32_t getUInt(const char* key, uint32_t defaultValue);
    size_t putUInt(const char* key, uint32_t value);

    bool clear();
};

#endif
