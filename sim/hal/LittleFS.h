#ifndef LITTLEFS_H
#define LITTLEFS_H

#include "FS.h"

class LittleFSClass {
public:
    bool begin(bool formatOnFail = false);
    size_t totalBytes();
    size_t usedBytes();
    File open(const char* path, uint8_t mode);
    bool exists(const char* path);
    bool remove(const char* path);
};

extern LittleFSClass LittleFS;

#endif
