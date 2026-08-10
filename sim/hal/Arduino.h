#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef bool boolean;
typedef uint8_t byte;

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

unsigned long millis();
void delay(unsigned long ms);

// Test-only hook: set millis() to an absolute value. Host build only.
// Used by the unit tests to simulate long uptimes (e.g. the 7-day
// capacity-test stale-recovery bound) without actually waiting.
void set_millis_for_test(unsigned long ms);

inline long random(long max) {
    if (max <= 0) return 0;
    return rand() % max;
}

inline long random(long min, long max) {
    if (min >= max) return min;
    return min + rand() % (max - min);
}

class SerialClass {
public:
    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void println(const char* s);
    void println();
};

extern SerialClass Serial;

// Minimal String class for host-side builds. The firmware's settings_manager.cpp
// uses String for Preferences::getString() return values. We provide just enough
// surface to compile: construction from const char*, c_str(), length(), and
// concatenation. No dynamic allocation tracking — the host build leaks small
// strings, which is acceptable for a test harness.
class String {
public:
    String() : buf_(nullptr), len_(0) {}
    String(const char* s) { *this = s; }
    String(const String& other) { *this = other.buf_; }
    ~String() { free(buf_); }

    String& operator=(const char* s) {
        free(buf_);
        if (s) {
            len_ = strlen(s);
            buf_ = (char*)malloc(len_ + 1);
            if (buf_) { memcpy(buf_, s, len_ + 1); }
        } else {
            buf_ = nullptr;
            len_ = 0;
        }
        return *this;
    }

    const char* c_str() const { return buf_ ? buf_ : ""; }
    size_t length() const { return len_; }
    bool operator==(const char* s) const { return strcmp(c_str(), s) == 0; }
    bool operator!=(const char* s) const { return strcmp(c_str(), s) != 0; }

    String operator+(const char* s) const {
        String result;
        size_t other_len = s ? strlen(s) : 0;
        result.buf_ = (char*)malloc(len_ + other_len + 1);
        if (result.buf_) {
            if (buf_) memcpy(result.buf_, buf_, len_);
            if (s) memcpy(result.buf_ + len_, s, other_len + 1);
            result.len_ = len_ + other_len;
        }
        return result;
    }

private:
    char* buf_;
    size_t len_;
};

#endif
