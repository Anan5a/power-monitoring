#ifndef ESP_SYSTEM_H
#define ESP_SYSTEM_H

#include <stdint.h>

// Minimal stub for ESP-IDF esp_system.h. The firmware's telemetry.cpp
// calls esp_reset_reason() to populate the reset_reason field in the
// TelemetrySnapshot. On the host we return ESP_RST_POWERON (0) so the
// sim always reports a clean power-on reset.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_RST_UNKNOWN   = 0,
    ESP_RST_POWERON   = 1,
    ESP_RST_EXT       = 2,
    ESP_RST_SW        = 3,
    ESP_RST_PANIC     = 4,
    ESP_RST_INT_WDT   = 5,
    ESP_RST_TASK_WDT  = 6,
    ESP_RST_WDT       = 7,
    ESP_RST_DEEPSLEEP = 8,
    ESP_RST_BROWNOUT  = 9,
    ESP_RST_SDIO      = 10,
    ESP_RST_USB       = 11,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);

#ifdef __cplusplus
}
#endif

#endif // ESP_SYSTEM_H
