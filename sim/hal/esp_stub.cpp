#include "ESP.h"

uint32_t ESPClass::getFreeHeap() {
    // Pretend we have plenty of heap. The thresholds in connectivity_manager.cpp
    // (13000/8192/4096/3072) all gate the publish path; we want everything
    // to run for the validation harness.
    return 200 * 1024;
}

ESPClass ESP;
