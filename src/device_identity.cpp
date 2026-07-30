#include "device_identity.h"
#include "settings_manager.h"
#include "log_serial.h"
#include <Arduino.h>
#include <string.h>

#if defined(ESP32) || defined(ESP32C3) || defined(ESP32S3)
#include <esp_efuse.h>
#endif

// NVS keys (stored in the main settings namespace via settings_manager)
static const char* NVS_KEY_SERIAL      = "dev_serial";
static const char* NVS_KEY_CRASH_CNT   = "crash_count";
static const char* NVS_KEY_CLEAN_BOOT  = "clean_boot_flag";

// Static cache (loaded once, never changes at runtime)
static char g_serial[DEVICE_SERIAL_MAX_LEN] = "";
static char g_hw_rev[DEVICE_HW_REV_MAX_LEN] = "";
static uint32_t g_crash_count = 0;
static bool g_initialized = false;

// ── Serial number generation ──────────────────────────────────────────
// Format: "PM-" + last 6 bytes of MAC as uppercase hex.
// Example: PM-AABBCCDDEEFF
static void generate_serial(char* out, size_t out_len) {
    uint8_t mac[6] = {0};
#if defined(ESP32) || defined(ESP32C3) || defined(ESP32S3)
    esp_efuse_mac_get_default(mac);
#endif
    snprintf(out, out_len, "PM-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void init_device_identity() {
    if (g_initialized) return;

    // ── Serial number ─────────────────────────────────────────────────
    // Try loading from NVS first. If not present, generate and persist.
    char buf[DEVICE_SERIAL_MAX_LEN] = "";
    if (settings_load_str(NVS_KEY_SERIAL, buf, sizeof(buf)) && strlen(buf) > 0) {
        strncpy(g_serial, buf, sizeof(g_serial) - 1);
        g_serial[sizeof(g_serial) - 1] = '\0';
    } else {
        generate_serial(g_serial, sizeof(g_serial));
        settings_save_str(NVS_KEY_SERIAL, g_serial);
        LOG_PRINT("[IDENTITY] generated serial: %s\n", g_serial);
    }

    // ── Hardware revision ─────────────────────────────────────────────
    // From compile-time define or NVS. The build system can set
    // -D DEVICE_HW_REV="rev1.0" per board.
#if defined(DEVICE_HW_REV)
    strncpy(g_hw_rev, DEVICE_HW_REV, sizeof(g_hw_rev) - 1);
    g_hw_rev[sizeof(g_hw_rev) - 1] = '\0';
#else
    strncpy(g_hw_rev, "rev0", sizeof(g_hw_rev) - 1);
    g_hw_rev[sizeof(g_hw_rev) - 1] = '\0';
#endif

    // ── Crash count ───────────────────────────────────────────────────
    // If the clean-shutdown flag is NOT set, the previous boot ended
    // unexpectedly (crash, power loss). Increment the crash counter.
    bool clean = false;
    settings_load_bool(NVS_KEY_CLEAN_BOOT, &clean);
    g_crash_count = 0;
    settings_load_u32(NVS_KEY_CRASH_CNT, &g_crash_count);

    if (!clean) {
        g_crash_count++;
        LOG_PRINT("[IDENTITY] unclean shutdown detected — crash count: %u\n",
                  (unsigned)g_crash_count);
    } else {
        // Clean shutdown: if the crash count was non-zero, log recovery.
        if (g_crash_count > 0) {
            LOG_PRINT("[IDENTITY] clean boot after %u crashes\n",
                      (unsigned)g_crash_count);
        }
    }

    // Persist the updated crash count and clear the clean-boot flag for
    // this boot cycle. mark_clean_shutdown() will set it on intentional
    // reboots.
    settings_save_u32(NVS_KEY_CRASH_CNT, g_crash_count);
    settings_save_bool(NVS_KEY_CLEAN_BOOT, false);

    // ── Boot loop detection ───────────────────────────────────────────
    // If crash count >= 5, enter safe mode: disable WiFi/BLE so the
    // device doesn't keep crashing on network init. The user must
    // factory-reset or flash new firmware to recover.
    if (g_crash_count >= 5) {
        LOG_PRINT("[IDENTITY] *** BOOT LOOP DETECTED (%u crashes) — entering safe mode ***\n",
                   (unsigned)g_crash_count);
        // Safe mode: skip WiFi/BLE init. The device will run with just
        // the serial console and sensor logging (RAM-only).
        // The flag is checked by main.cpp's setup().
    }

    g_initialized = true;
}

const char* get_device_serial() {
    if (!g_initialized) init_device_identity();
    return g_serial;
}

const char* get_device_hw_rev() {
    if (!g_initialized) init_device_identity();
    return g_hw_rev;
}

uint32_t get_crash_count() {
    if (!g_initialized) init_device_identity();
    return g_crash_count;
}

void reset_crash_count() {
    g_crash_count = 0;
    settings_save_u32(NVS_KEY_CRASH_CNT, 0);
    LOG_PRINTLN("[IDENTITY] crash count reset");
}

void mark_clean_shutdown() {
    settings_save_bool(NVS_KEY_CLEAN_BOOT, true);
    LOG_PRINTLN("[IDENTITY] clean shutdown marked");
}
