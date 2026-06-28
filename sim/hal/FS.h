#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

#define FILE_READ 0
#define FILE_WRITE 1
#define FILE_APPEND 2

class File {
public:
    File() : valid(false) {}

    operator bool() const { return valid; }

    size_t write(const uint8_t* buf, size_t len) {
        (void)buf;
        return len;
    }

    size_t read(uint8_t* buf, size_t len) {
        (void)buf;
        (void)len;
        return 0;
    }

    size_t size() const { return 0; }

    void close() { valid = false; }

private:
    bool valid;
};

#endif
