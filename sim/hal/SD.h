#ifndef SD_H
#define SD_H

#include <stdint.h>
#include <stddef.h>

// Minimal stub for the Arduino SD library. The sim's data_logger.cpp
// includes <SD.h> for the SD card overflow path. On the host we provide
// no-op stubs so the module compiles; the sim does not exercise SD I/O.

class File {
public:
    File() : _open(false) {}
    operator bool() { return _open; }
    size_t write(const uint8_t*, size_t len) { (void)len; return 0; }
    size_t write(uint8_t) { return 0; }
    int available() { return 0; }
    int read() { return -1; }
    size_t read(uint8_t*, size_t) { return 0; }
    void flush() {}
    bool seek(uint32_t) { return false; }
    size_t position() { return 0; }
    size_t size() { return 0; }
    void close() { _open = false; }
    const char* name() { return ""; }
    bool isDirectory() { return false; }
    File openNextFile() { return File(); }
    void rewindDirectory() {}
    // Print API used by data_logger.cpp CSV writing
    size_t print(const char* s) { (void)s; return 0; }
    size_t print(int n) { (void)n; return 0; }
    size_t print(unsigned int n) { (void)n; return 0; }
    size_t print(float f, int decimals = 2) { (void)f; (void)decimals; return 0; }
    size_t println(const char* s) { (void)s; return 0; }
    size_t println() { return 0; }
    size_t println(float f, int decimals = 2) { (void)f; (void)decimals; return 0; }
    size_t println(int n) { (void)n; return 0; }
    size_t println(unsigned int n) { (void)n; return 0; }
    bool _open;
};

class SDClass {
public:
    bool begin(uint8_t csPin = 4) { (void)csPin; return false; }
    bool begin(uint8_t csPin, uint32_t freq) { (void)csPin; (void)freq; return false; }
    File open(const char* path, uint8_t mode = 0) {
        (void)path; (void)mode;
        return File();
    }
    bool exists(const char* path) { (void)path; return false; }
    bool remove(const char* path) { (void)path; return false; }
    bool mkdir(const char* path) { (void)path; return false; }
    bool rmdir(const char* path) { (void)path; return false; }
    uint64_t totalBytes() { return 0; }
    uint64_t usedBytes() { return 0; }
    uint64_t cardSize() { return 0; }
    int freeSpaceMB() { return 0; }
};

extern SDClass SD;

// File open modes
#define FILE_READ    0
#define FILE_WRITE   1
#define FILE_APPEND  2

#endif // SD_H
