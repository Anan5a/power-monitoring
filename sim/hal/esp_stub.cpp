#include "ESP.h"
#include "esp_system.h"

uint32_t ESPClass::getFreeHeap() {
    // Pretend we have plenty of heap. The thresholds in connectivity_manager.cpp
    // (13000/8192/4096/3072) all gate the publish path; we want everything
    // to run for the validation harness.
    return 200 * 1024;
}

uint32_t ESPClass::getMinFreeHeap() {
    return 180 * 1024;
}

esp_reset_reason_t esp_reset_reason(void) {
    return ESP_RST_POWERON;
}

ESPClass ESP;
