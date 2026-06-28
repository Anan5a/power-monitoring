#include "LittleFS.h"

bool LittleFSClass::begin(bool formatOnFail) {
    (void)formatOnFail;
    return false;
}

size_t LittleFSClass::totalBytes() {
    return 0;
}

size_t LittleFSClass::usedBytes() {
    return 0;
}

File LittleFSClass::open(const char* path, uint8_t mode) {
    (void)path;
    (void)mode;
    return File();
}

bool LittleFSClass::exists(const char* path) {
    (void)path;
    return false;
}

bool LittleFSClass::remove(const char* path) {
    (void)path;
    return false;
}

LittleFSClass LittleFS;
