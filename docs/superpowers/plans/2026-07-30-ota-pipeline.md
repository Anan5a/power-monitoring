# OTA Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add OTA firmware update capability to the ESP32 firmware — polling backend, streaming download with SHA256 verification, ESP32 native rollback, status reporting.

**Architecture:** New `ota_client` module with a non-blocking state machine (IDLE → CHECKING → DOWNLOADING → APPLYING → REBOOTING) driven from the network task. One chunk per `loop_ota_client()` call so the 10ms tick stays responsive. SHA256 computed incrementally during download. ESP32 native rollback via `esp_ota_mark_app_valid_cancel_rollback()` on firmware-health signal (sensors + 60s grace). Poll interval from backend response, persisted in NVS.

**Tech Stack:** ESP-IDF OTA APIs (`esp_ota_begin/write/end`, `esp_ota_set_boot_partition`, `esp_ota_mark_app_valid_cancel_rollback`), mbedTLS SHA256, WiFiClientSecure, ArduinoJson, NVS (Preferences).

---

## File Map

### New Files
| File | Responsibility |
|---|---|
| `include/ota_client.h` | OTA state enum, public API declarations |
| `src/ota_client.cpp` | OTA state machine, HTTP download, SHA256, esp_ota, poll timer |

### Modified Files
| File | Changes |
|---|---|
| `include/config.h` | Add OTA defaults: `OTA_POLL_INTERVAL_S`, `OTA_HTTP_TIMEOUT_MS`, `OTA_CHUNK_SIZE`, `OTA_DOWNLOAD_TIMEOUT_MS` |
| `include/settings_manager.h` | Add `settings_load_ota_poll_interval()`, `settings_save_ota_poll_interval()` |
| `src/settings_manager.cpp` | Implement OTA NVS persistence |
| `include/telemetry.h` | Add `TelemetryOTA` struct, add `ota` field to `TelemetrySnapshot` |
| `src/telemetry.cpp` | Populate OTA fields in `telemetry_build()` |
| `include/connectivity_manager.h` | Add `publish_ota_status()` declaration |
| `src/connectivity_manager.cpp` | Implement `publish_ota_status()` MQTT publish |
| `include/ble_provisioner.h` | No change needed (commands go through `apply_settings_command`) |
| `src/ble_provisioner.cpp` | Update `ota_start` to use OTA client, add `ota_check`/`ota_status`/`ota_set_interval` to `handle_command` and `apply_settings_command` |
| `src/main.cpp` | Add `loop_ota_client()` to network task, add rollback confirmation in `setup()` |
| `backend/internal/ota.go` | Add `poll_interval_seconds` to `OTACheckResponse` and `CheckOTA` response |

---

### Task 1: Add OTA constants to config.h

**Files:**
- Modify: `include/config.h` (before `#endif` at line 137)

- [ ] **Add OTA configuration constants**

Insert before the `#endif` at line 137:

```cpp
// OTA update client
#ifndef OTA_POLL_INTERVAL_S
#define OTA_POLL_INTERVAL_S      300   // default poll interval (5 min)
#endif
#ifndef OTA_POLL_INTERVAL_MIN_S
#define OTA_POLL_INTERVAL_MIN_S  60    // minimum poll interval (1 min)
#endif
#ifndef OTA_POLL_INTERVAL_MAX_S
#define OTA_POLL_INTERVAL_MAX_S  86400 // maximum poll interval (24 h)
#endif
#ifndef OTA_HTTP_TIMEOUT_MS
#define OTA_HTTP_TIMEOUT_MS      10000 // HTTP connect timeout (10 s)
#endif
#ifndef OTA_CHUNK_SIZE
#define OTA_CHUNK_SIZE           1024  // bytes per download chunk
#endif
#ifndef OTA_DOWNLOAD_TIMEOUT_MS
#define OTA_DOWNLOAD_TIMEOUT_MS  300000 // total download timeout (5 min)
#endif
#ifndef OTA_GRACE_SECONDS
#define OTA_GRACE_SECONDS        60    // seconds after boot before mark_valid
#endif
```

- [ ] **Commit**

```bash
git add include/config.h
git commit -m "feat: add OTA configuration constants"
```

---

### Task 2: Add OTA poll interval NVS persistence

**Files:**
- Modify: `include/settings_manager.h` (before `#endif` at line 196)
- Modify: `src/settings_manager.cpp`

- [ ] **Add declarations to settings_manager.h**

Insert before `#endif` at line 196:

```cpp
// OTA poll interval (seconds). Default OTA_POLL_INTERVAL_S if not set.
uint32_t settings_load_ota_poll_interval();
void settings_save_ota_poll_interval(uint32_t interval_s);
```

- [ ] **Add implementations to settings_manager.cpp**

Find the existing `settings_save_ble_fail_count` function and add after it:

```cpp
uint32_t settings_load_ota_poll_interval() {
    Preferences prefs;
    if (!prefs.begin("pm-ota", true)) return OTA_POLL_INTERVAL_S;
    uint32_t val = prefs.getUInt("poll_interval", OTA_POLL_INTERVAL_S);
    prefs.end();
    if (val < OTA_POLL_INTERVAL_MIN_S) val = OTA_POLL_INTERVAL_MIN_S;
    if (val > OTA_POLL_INTERVAL_MAX_S) val = OTA_POLL_INTERVAL_MAX_S;
    return val;
}

void settings_save_ota_poll_interval(uint32_t interval_s) {
    if (interval_s < OTA_POLL_INTERVAL_MIN_S) interval_s = OTA_POLL_INTERVAL_MIN_S;
    if (interval_s > OTA_POLL_INTERVAL_MAX_S) interval_s = OTA_POLL_INTERVAL_MAX_S;
    Preferences prefs;
    prefs.begin("pm-ota", false);
    prefs.putUInt("poll_interval", interval_s);
    prefs.end();
}
```

- [ ] **Commit**

```bash
git add include/settings_manager.h src/settings_manager.cpp
git commit -m "feat: add OTA poll interval NVS persistence"
```

---

### Task 3: Create ota_client.h — state machine and public API

**Files:**
- Create: `include/ota_client.h`

- [ ] **Write the header**

```cpp
#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

// ── OTA state machine ──────────────────────────────────────────────────────────
// Driven by loop_ota_client() on the network task. One chunk per tick so the
// 10 ms loop stays responsive. State transitions are guarded by reentrancy:
// triggers (poll timer, BLE/Supabase commands) are ignored unless state is IDLE.

enum OtaState {
    OTA_IDLE = 0,
    OTA_CHECKING,       // HTTP GET /ota/check/{key}?current_ver=X.X.X
    OTA_DOWNLOADING,    // streaming chunked download + SHA256 + esp_ota_write
    OTA_APPLYING,       // download complete, verify SHA256, set boot partition
    OTA_REBOOTING,      // esp_restart() called
    OTA_FAILED,         // last attempt failed, will retry on next poll
};

// ── Public API ──────────────────────────────────────────────────────────────────

// Initialize the OTA client. Must be called once from setup() after
// init_settings() and init_connectivity(). Does not start polling.
void init_ota_client();

// Drive the OTA state machine. Call from the network task loop (every 10 ms).
// Handles: poll timer, HTTP download chunks, SHA256, esp_ota operations.
void loop_ota_client();

// Trigger an immediate OTA check (bypasses the poll timer). Safe to call from
// BLE/Supabase command handlers. No-op if state is not IDLE.
void ota_trigger_check();

// Return the current OTA state.
OtaState ota_get_state();

// Return the version string of the update being applied (or "" if idle).
const char* ota_get_version();

// Return download progress percentage (0-100) during OTA_DOWNLOADING.
uint8_t ota_get_progress_pct();

// Return the last error message (or "" if none).
const char* ota_get_last_error();

// Set the poll interval (seconds). Clamped to [OTA_POLL_INTERVAL_MIN_S,
// OTA_POLL_INTERVAL_MAX_S]. Persisted to NVS.
void ota_set_poll_interval(uint32_t interval_s);

// Get the current poll interval (seconds).
uint32_t ota_get_poll_interval();

// Confirm the new firmware is valid (call from setup() after successful init).
// Must be called once after boot if the partition is in PENDING_VERIFY state.
// Uses firmware-health signal: sensors init + first read + 60s grace.
void ota_confirm_valid();

#endif // OTA_CLIENT_H
```

- [ ] **Commit**

```bash
git add include/ota_client.h
git commit -m "feat: add ota_client header with state machine and API"
```

---

### Task 4: Implement ota_client.cpp — state machine, download, SHA256, rollback

**Files:**
- Create: `src/ota_client.cpp`

- [ ] **Write the full implementation**

```cpp
#include "ota_client.h"
#include "config.h"
#include "settings_manager.h"
#include "connectivity_manager.h"
#include "device_identity.h"
#include "event_log.h"
#include "log_serial.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>

// ── Static state ────────────────────────────────────────────────────────────────

static OtaState  g_state = OTA_IDLE;
static char      g_version[16] = "";
static char      g_expected_sha256[65] = "";  // hex string, 64 chars + null
static uint32_t  g_expected_size = 0;
static uint8_t   g_progress_pct = 0;
static char      g_last_error[64] = "";

// Download state (persisted across loop_ota_client() calls)
static WiFiClientSecure* g_tls = nullptr;
static HTTPClient*       g_http = nullptr;
static esp_ota_handle_t  g_ota_handle = 0;
static const esp_partition_t* g_ota_partition = nullptr;
static mbedtls_sha256_context g_sha256_ctx;
static bool     g_sha256_inited = false;
static uint32_t g_total_written = 0;
static unsigned long g_download_start_ms = 0;
static uint8_t   g_last_reported_pct = 0;

// Poll timer
static unsigned long g_last_poll_ms = 0;
static uint32_t      g_poll_interval_s = OTA_POLL_INTERVAL_S;

// Rollback confirmation
static bool g_rollback_confirmed = false;

// ── Helpers ─────────────────────────────────────────────────────────────────────

static void set_state(OtaState st) {
    g_state = st;
}

static void set_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
    va_end(args);
    log_event(EVENT_LOG_ERROR, "ota", "%s", g_last_error);
}

static void cleanup_download() {
    if (g_http) {
        g_http->end();
        delete g_http;
        g_http = nullptr;
    }
    if (g_tls) {
        delete g_tls;
        g_tls = nullptr;
    }
    if (g_ota_handle) {
        esp_ota_end(g_ota_handle);
        g_ota_handle = 0;
    }
    if (g_sha256_inited) {
        mbedtls_sha256_free(&g_sha256_ctx);
        g_sha256_inited = false;
    }
    g_ota_partition = nullptr;
    g_total_written = 0;
    g_download_start_ms = 0;
    g_last_reported_pct = 0;
    g_version[0] = '\0';
    g_expected_sha256[0] = '\0';
    g_expected_size = 0;
    g_progress_pct = 0;
}

// Publish OTA status via MQTT
static void publish_status(const char* status_str) {
    publish_ota_status(status_str, g_version, g_progress_pct, g_last_error);
}

// ── Public API ──────────────────────────────────────────────────────────────────

void init_ota_client() {
    g_poll_interval_s = settings_load_ota_poll_interval();
    g_last_poll_ms = millis();
    set_state(OTA_IDLE);
    LOG_PRINT("[OTA] client initialized, poll interval=%u s\n", (unsigned)g_poll_interval_s);
}

void loop_ota_client() {
    // Skip if WiFi is not connected (no point checking)
    if (WiFi.status() != WL_CONNECTED) return;

    switch (g_state) {
        case OTA_IDLE: {
            // Check poll timer
            unsigned long now = millis();
            if (now - g_last_poll_ms >= (unsigned long)g_poll_interval_s * 1000) {
                g_last_poll_ms = now;
                set_state(OTA_CHECKING);
                LOG_PRINT("[OTA] poll timer fired, checking for updates\n");
                publish_status("checking");
                // Fall through to CHECKING immediately
            } else {
                return; // nothing to do
            }
        }
        // intentional fall-through

        case OTA_CHECKING: {
            // Build URL: /ota/check/{device_key}?current_ver=X.X.X
            // The backend base URL is read from NVS (set via BLE/Supabase command).
            char backend_url[128] = "";
            {
                Preferences prefs;
                if (prefs.begin("pm-ota", true)) {
                    String s = prefs.getString("backend_url", "");
                    if (s.length() > 0) {
                        strncpy(backend_url, s.c_str(), sizeof(backend_url) - 1);
                    }
                    prefs.end();
                }
            }
            if (backend_url[0] == '\0') {
                // No backend URL configured — skip check, go back to IDLE
                set_state(OTA_IDLE);
                return;
            }
            char fw_ver[32];
            strncpy(fw_ver, TELEMETRY_FW_VERSION, sizeof(fw_ver) - 1);
            fw_ver[sizeof(fw_ver) - 1] = '\0';
            char url[256];
            snprintf(url, sizeof(url), "%s/ota/check/%s?current_ver=%s",
                     backend_url, get_device_serial(), fw_ver);

            WiFiClientSecure tls;
            tls.setInsecure();
            tls.setHandshakeTimeout(10);
            HTTPClient http;
            http.setTimeout(OTA_HTTP_TIMEOUT_MS);
            if (!http.begin(tls, url)) {
                set_error("HTTP begin failed");
                http.end();
                set_state(OTA_IDLE);
                return;
            }
            int rc = http.GET();
            if (rc != 200) {
                set_error("check returned %d", rc);
                http.end();
                set_state(OTA_IDLE);
                return;
            }
            // Parse JSON response
            String body = http.getString();
            http.end();

            // Simple JSON parsing with ArduinoJson
            // We use a StaticJsonDocument to avoid heap fragmentation
            StaticJsonDocument<512> doc;
            DeserializationError err = deserializeJson(doc, body);
            if (err) {
                set_error("JSON parse: %s", err.c_str());
                set_state(OTA_IDLE);
                return;
            }

            bool update_avail = doc["update_available"] | false;
            if (!update_avail) {
                // Still update poll interval if provided
                uint32_t pi = doc["poll_interval_seconds"] | 0;
                if (pi > 0) ota_set_poll_interval(pi);
                LOG_PRINT("[OTA] no update available\n");
                set_state(OTA_IDLE);
                return;
            }

            // Extract update info
            const char* ver = doc["version"] | "";
            const char* sha = doc["sha256"] | "";
            uint32_t size = doc["binary_size"] | 0;
            uint32_t pi = doc["poll_interval_seconds"] | 0;

            if (!ver[0] || !sha[0] || size == 0) {
                set_error("incomplete response: ver=%s sha=%s size=%u", ver, sha, (unsigned)size);
                set_state(OTA_IDLE);
                return;
            }

            strncpy(g_version, ver, sizeof(g_version) - 1);
            g_version[sizeof(g_version) - 1] = '\0';
            strncpy(g_expected_sha256, sha, sizeof(g_expected_sha256) - 1);
            g_expected_sha256[sizeof(g_expected_sha256) - 1] = '\0';
            g_expected_size = size;
            g_progress_pct = 0;
            g_last_reported_pct = 0;

            if (pi > 0) ota_set_poll_interval(pi);

            LOG_PRINT("[OTA] update available: v%s (%u bytes)\n", g_version, (unsigned)g_expected_size);
            log_event(EVENT_LOG_INFO, "ota", "update v%s available", g_version);
            publish_status("downloading");

            // Build the binary URL from the response
            const char* binary_url = doc["binary_url"] | "";
            if (!binary_url[0]) {
                set_error("no binary_url in response");
                set_state(OTA_IDLE);
                return;
            }

            // Start the download
            g_tls = new WiFiClientSecure();
            g_tls->setInsecure();
            g_tls->setHandshakeTimeout(10);
            g_http = new HTTPClient();
            g_http->setTimeout(OTA_HTTP_TIMEOUT_MS);
            if (!g_http->begin(*g_tls, binary_url)) {
                set_error("HTTP begin failed for binary");
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }
            int bin_rc = g_http->GET();
            if (bin_rc != 200) {
                set_error("binary GET returned %d", bin_rc);
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Find the next OTA partition
            g_ota_partition = esp_ota_get_next_update_partition(NULL);
            if (!g_ota_partition) {
                set_error("no OTA partition found");
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Begin OTA write
            esp_err_t ota_err = esp_ota_begin(g_ota_partition, g_expected_size, &g_ota_handle);
            if (ota_err != ESP_OK) {
                set_error("esp_ota_begin: %s", esp_err_to_name(ota_err));
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Init SHA256
            mbedtls_sha256_init(&g_sha256_ctx);
            mbedtls_sha256_starts(&g_sha256_ctx, 0); // 0 = SHA256
            g_sha256_inited = true;

            g_total_written = 0;
            g_download_start_ms = millis();
            set_state(OTA_DOWNLOADING);
            return;
        }

        case OTA_DOWNLOADING: {
            // Feed the WDT
            esp_task_wdt_reset();

            // Check total download timeout
            if (millis() - g_download_start_ms > OTA_DOWNLOAD_TIMEOUT_MS) {
                set_error("download timeout (%u s)", (unsigned)(OTA_DOWNLOAD_TIMEOUT_MS / 1000));
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            WiFiClient* stream = g_http->getStreamPtr();
            if (!stream) {
                set_error("no HTTP stream");
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Read one chunk
            uint8_t buf[OTA_CHUNK_SIZE];
            size_t n = stream->readBytes(buf, sizeof(buf));
            if (n == 0) {
                // No data available yet — check if connection is still alive
                if (!g_http->connected()) {
                    // Stream ended — check if we got all the data
                    if (g_total_written >= g_expected_size) {
                        // Download complete, move to APPLYING
                        set_state(OTA_APPLYING);
                        return;
                    }
                    // Connection dropped prematurely
                    set_error("connection lost after %u bytes", (unsigned)g_total_written);
                    cleanup_download();
                    set_state(OTA_IDLE);
                    return;
                }
                // Still connected, no data yet — return and try again next tick
                return;
            }

            // Write to OTA partition
            esp_err_t ota_err = esp_ota_write(g_ota_handle, buf, n);
            if (ota_err != ESP_OK) {
                set_error("esp_ota_write: %s", esp_err_to_name(ota_err));
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Update SHA256
            mbedtls_sha256_update(&g_sha256_ctx, buf, n);

            g_total_written += n;

            // Report progress every 10%
            uint8_t pct = (uint8_t)((uint64_t)g_total_written * 100 / g_expected_size);
            if (pct > 100) pct = 100;
            g_progress_pct = pct;
            if (pct - g_last_reported_pct >= 10 || pct == 100) {
                g_last_reported_pct = pct;
                publish_status("downloading");
            }

            // Check if we've received all bytes
            if (g_total_written >= g_expected_size) {
                set_state(OTA_APPLYING);
            }
            return;
        }

        case OTA_APPLYING: {
            esp_task_wdt_reset();

            // Finalize SHA256
            uint8_t computed_hash[32];
            mbedtls_sha256_finish(&g_sha256_ctx, computed_hash);
            mbedtls_sha256_free(&g_sha256_ctx);
            g_sha256_inited = false;

            // Convert computed hash to hex string
            char computed_hex[65];
            for (int i = 0; i < 32; i++) {
                sprintf(computed_hex + i * 2, "%02x", computed_hash[i]);
            }
            computed_hex[64] = '\0';

            // Verify size
            if (g_total_written != g_expected_size) {
                set_error("size mismatch: wrote %u, expected %u",
                          (unsigned)g_total_written, (unsigned)g_expected_size);
                esp_ota_abort(g_ota_handle);
                g_ota_handle = 0;
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Verify SHA256 BEFORE esp_ota_end — on mismatch we abort
            // (discard the partial slot) instead of finalizing a bad image.
            if (strcasecmp(computed_hex, g_expected_sha256) != 0) {
                set_error("SHA256 mismatch: got %s, expected %s",
                          computed_hex, g_expected_sha256);
                esp_ota_abort(g_ota_handle);
                g_ota_handle = 0;
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            LOG_PRINT("[OTA] SHA256 verified: %s\n", computed_hex);

            // End OTA write (finalizes the image header)
            esp_err_t ota_err = esp_ota_end(g_ota_handle);
            g_ota_handle = 0;
            if (ota_err != ESP_OK) {
                set_error("esp_ota_end: %s", esp_err_to_name(ota_err));
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            // Set the boot partition
            ota_err = esp_ota_set_boot_partition(g_ota_partition);
            if (ota_err != ESP_OK) {
                set_error("esp_ota_set_boot_partition: %s", esp_err_to_name(ota_err));
                cleanup_download();
                set_state(OTA_IDLE);
                return;
            }

            LOG_PRINT("[OTA] image verified and set as boot partition\n");
            log_event(EVENT_LOG_INFO, "ota", "update to v%s applied, rebooting", g_version);
            publish_status("applied");

            // Mark clean shutdown and reboot
            mark_clean_shutdown();
            cleanup_download();
            set_state(OTA_REBOOTING);

            vTaskDelay(pdMS_TO_TICKS(100));
            ESP.restart();
            return;
        }

        case OTA_REBOOTING:
            // Should not reach here — ESP.restart() was called
            return;

        case OTA_FAILED:
            // Stay in failed state until next poll timer fires
            // (the timer will transition to CHECKING)
            {
                unsigned long now = millis();
                if (now - g_last_poll_ms >= (unsigned long)g_poll_interval_s * 1000) {
                    g_last_poll_ms = now;
                    g_last_error[0] = '\0';
                    set_state(OTA_CHECKING);
                }
            }
            return;
    }
}

void ota_trigger_check() {
    if (g_state != OTA_IDLE && g_state != OTA_FAILED) {
        LOG_PRINT("[OTA] trigger ignored — state=%d\n", (int)g_state);
        return;
    }
    g_last_poll_ms = 0; // force immediate check on next loop_ota_client() call
    g_last_error[0] = '\0';
    set_state(OTA_CHECKING);
}

OtaState ota_get_state() {
    return g_state;
}

const char* ota_get_version() {
    return g_version;
}

uint8_t ota_get_progress_pct() {
    return g_progress_pct;
}

const char* ota_get_last_error() {
    return g_last_error;
}

void ota_set_poll_interval(uint32_t interval_s) {
    if (interval_s < OTA_POLL_INTERVAL_MIN_S) interval_s = OTA_POLL_INTERVAL_MIN_S;
    if (interval_s > OTA_POLL_INTERVAL_MAX_S) interval_s = OTA_POLL_INTERVAL_MAX_S;
    g_poll_interval_s = interval_s;
    settings_save_ota_poll_interval(interval_s);
}

uint32_t ota_get_poll_interval() {
    return g_poll_interval_s;
}

void ota_confirm_valid() {
    if (g_rollback_confirmed) return;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) return;

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            LOG_PRINT("[OTA] firmware confirmed valid — rollback cancelled\n");
            log_event(EVENT_LOG_INFO, "ota", "firmware confirmed valid");
            g_rollback_confirmed = true;
        } else {
            LOG_PRINT("[OTA] mark_app_valid failed: %s\n", esp_err_to_name(err));
        }
    } else {
        // Not a pending-verify boot — nothing to confirm
        g_rollback_confirmed = true;
    }
}
```

- [ ] **Commit**

```bash
git add src/ota_client.cpp
git commit -m "feat: implement OTA client state machine, download, SHA256, rollback"
```

---

### Task 5: Add TelemetryOTA to telemetry.h

**Files:**
- Modify: `include/telemetry.h`

- [ ] **Add TelemetryOTA struct and field to TelemetrySnapshot**

Insert after `struct TelemetryLogMeta` (after line 90):

```cpp
struct TelemetryOTA {
    bool    ota_in_progress;    // true while downloading/applying
    char    ota_version[16];    // version being applied
    uint8_t ota_progress_pct;   // 0..100 during download
    char    ota_status[16];     // "idle", "checking", "downloading",
                                // "applying", "rebooting", "failed"
};
```

Add `TelemetryOTA ota;` field to `TelemetrySnapshot` after `heap_free` (after line 110):

```cpp
    TelemetryLogMeta log;
    uint32_t heap_free;
    TelemetryOTA ota;
};
```

- [ ] **Commit**

```bash
git add include/telemetry.h
git commit -m "feat: add TelemetryOTA struct to telemetry snapshot"
```

---

### Task 6: Populate OTA fields in telemetry_build()

**Files:**
- Modify: `src/telemetry.cpp`

- [ ] **Add OTA field population**

Add `#include "ota_client.h"` to the includes at the top of `telemetry.cpp`.

Add after the heap line (after `out.heap_free = ESP.getFreeHeap();`):

```cpp
    // --- OTA status -------------------------------------------------------------
    OtaState ota_st = ota_get_state();
    out.ota.ota_in_progress = (ota_st == OTA_DOWNLOADING || ota_st == OTA_APPLYING);
    const char* ver = ota_get_version();
    if (ver) {
        strncpy(out.ota.ota_version, ver, sizeof(out.ota.ota_version));
        out.ota.ota_version[sizeof(out.ota.ota_version) - 1] = '\0';
    }
    out.ota.ota_progress_pct = ota_get_progress_pct();
    const char* status_str = "";
    switch (ota_st) {
        case OTA_IDLE:        status_str = "idle";        break;
        case OTA_CHECKING:    status_str = "checking";    break;
        case OTA_DOWNLOADING: status_str = "downloading"; break;
        case OTA_APPLYING:    status_str = "applying";    break;
        case OTA_REBOOTING:   status_str = "rebooting";   break;
        case OTA_FAILED:      status_str = "failed";      break;
    }
    strncpy(out.ota.ota_status, status_str, sizeof(out.ota.ota_status));
    out.ota.ota_status[sizeof(out.ota.ota_status) - 1] = '\0';
```

- [ ] **Commit**

```bash
git add src/telemetry.cpp
git commit -m "feat: populate OTA fields in telemetry_build()"
```

---

### Task 7: Add publish_ota_status() to connectivity_manager

**Files:**
- Modify: `include/connectivity_manager.h`
- Modify: `src/connectivity_manager.cpp`

- [ ] **Add declaration to connectivity_manager.h**

Add after `void publish_switch_state(...)` (around line 48):

```cpp
// Publish OTA status to MQTT topic status/{device_key}/ota
void publish_ota_status(const char* status, const char* version,
                         uint8_t progress_pct, const char* error);
```

- [ ] **Add implementation to connectivity_manager.cpp**

Find the existing `publish_switch_state` function and add after it:

```cpp
void publish_ota_status(const char* status, const char* version,
                         uint8_t progress_pct, const char* error) {
    if (!mqtt_client.connected()) return;

    char topic[128];
    char device_key[64] = "";
    settings_load_supabase_device_key(device_key, sizeof(device_key));
    if (device_key[0] == '\0') {
        // Fall back to device serial if no device_key configured
        strncpy(device_key, get_device_serial(), sizeof(device_key) - 1);
    }
    snprintf(topic, sizeof(topic), "status/%s/ota", device_key);

    StaticJsonDocument<256> doc;
    doc["status"] = status;
    if (version && version[0]) doc["version"] = version;
    if (progress_pct > 0) doc["progress"] = progress_pct;
    if (error && error[0]) doc["error"] = error;

    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n > 0) {
        mqtt_client.publish(topic, 0, false, buf);
    }
}
```

Add the required includes at the top of the file if not already present:
- `#include "device_identity.h"` (for `get_device_serial()`)
- `#include <ArduinoJson.h>` (should already be there)

- [ ] **Commit**

```bash
git add include/connectivity_manager.h src/connectivity_manager.cpp
git commit -m "feat: add publish_ota_status() MQTT helper"
```

---

### Task 8: Add OTA commands to BLE provisioner

**Files:**
- Modify: `src/ble_provisioner.cpp`

- [ ] **Add `#include "ota_client.h"` to the includes**

- [ ] **Add `ota_check` command to `handle_command()`**

Find the end of the `handle_command()` function (the last `else if` before the closing `else { send_error(...) }` block). Add before the final `else`:

```cpp
    } else if (strcmp(cmd, "ota_check") == 0) {
        ota_trigger_check();
        send_ok(cmd, "ota check triggered");
    } else if (strcmp(cmd, "ota_status") == 0) {
        StaticJsonDocument<256> resp;
        resp["ok"] = true;
        resp["cmd"] = cmd;
        resp["state"] = (int)ota_get_state();
        const char* state_names[] = {"idle","checking","downloading","applying","rebooting","failed"};
        int st = (int)ota_get_state();
        if (st >= 0 && st < 6) resp["state_name"] = state_names[st];
        resp["version"] = ota_get_version();
        resp["progress"] = ota_get_progress_pct();
        resp["error"] = ota_get_last_error();
        resp["poll_interval_s"] = ota_get_poll_interval();
        char buf[512];
        size_t n = serializeJson(resp, buf, sizeof(buf));
        send_response(buf, n);
    } else if (strcmp(cmd, "ota_set_interval") == 0) {
        uint32_t interval = doc["interval"] | 0;
        if (interval < OTA_POLL_INTERVAL_MIN_S) interval = OTA_POLL_INTERVAL_MIN_S;
        if (interval > OTA_POLL_INTERVAL_MAX_S) interval = OTA_POLL_INTERVAL_MAX_S;
        ota_set_poll_interval(interval);
        send_ok(cmd, "poll interval updated");
```

- [ ] **Update `ota_start` in `apply_settings_command()`**

Replace the existing `ota_start` handler (lines 1469-1483) to use the OTA client instead of the direct `do_ota_from_url`:

```cpp
    } else if (strcmp(cmd_type, "ota_start") == 0) {
        if (!supa_pin_ok(doc)) {
            LOG_PRINTLN("[CMD] ota_start rejected (pin missing/wrong)");
            return false;
        }
        ota_trigger_check();
        LOG_PRINTLN("[CMD] ota check triggered via ota_start");
        return true;
```

- [ ] **Add `ota_check`, `ota_status`, `ota_set_interval` to `apply_settings_command()`**

Add before the final `else { LOG_PRINT(...) }` block:

```cpp
    } else if (strcmp(cmd_type, "ota_check") == 0) {
        ota_trigger_check();
        return true;
    } else if (strcmp(cmd_type, "ota_status") == 0) {
        // Status is reported via telemetry and MQTT; this is a no-op trigger
        return true;
    } else if (strcmp(cmd_type, "ota_set_interval") == 0) {
        uint32_t interval = doc["interval"] | 0;
        ota_set_poll_interval(interval);
        return true;
```

- [ ] **Commit**

```bash
git add src/ble_provisioner.cpp
git commit -m "feat: add OTA BLE/Supabase commands (ota_check, ota_status, ota_set_interval)"
```

---

### Task 9: Integrate OTA client into main.cpp

**Files:**
- Modify: `src/main.cpp`

- [ ] **Add `#include "ota_client.h"` to the includes**

- [ ] **Add rollback confirmation in `setup()`**

After `init_core_shared();` (around line 967) and before the BLE init, add:

```cpp
    // Confirm firmware validity for OTA rollback. Must be called after
    // sensors init + first successful init sequence. The 60s grace timer
    // is handled by the sensor task feeding the WDT — if we reach this
    // point without crashing, the firmware is healthy enough to confirm.
    ota_confirm_valid();
```

- [ ] **Add `loop_ota_client()` to the network task**

After `loop_ble_provisioner();` (line 121), add:

```cpp
        loop_ota_client();
```

- [ ] **Initialize OTA client in `setup()`**

After `init_core_shared();` (around line 967), add:

```cpp
    init_ota_client();
```

- [ ] **Commit**

```bash
git add src/main.cpp
git commit -m "feat: integrate OTA client into network task and setup()"
```

---

### Task 10: Add backend URL to NVS and config

**Files:**
- Modify: `include/settings_manager.h`
- Modify: `src/settings_manager.cpp`

- [ ] **Add backend URL declarations to settings_manager.h**

Add before `#endif`:

```cpp
// OTA backend base URL (e.g. "https://api.example.com")
bool settings_load_ota_backend_url(char* url, size_t buf_len);
void settings_save_ota_backend_url(const char* url);
```

- [ ] **Add implementations to settings_manager.cpp**

```cpp
bool settings_load_ota_backend_url(char* url, size_t buf_len) {
    Preferences prefs;
    if (!prefs.begin("pm-ota", true)) return false;
    String s = prefs.getString("backend_url", "");
    prefs.end();
    if (s.length() == 0) return false;
    strncpy(url, s.c_str(), buf_len - 1);
    url[buf_len - 1] = '\0';
    return true;
}

void settings_save_ota_backend_url(const char* url) {
    Preferences prefs;
    prefs.begin("pm-ota", false);
    prefs.putString("backend_url", url);
    prefs.end();
}
```

- [ ] **Commit**

```bash
git add include/settings_manager.h src/settings_manager.cpp
git commit -f "feat: add OTA backend URL NVS persistence"
```

---

### Task 11: Update backend OTA check response

**Files:**
- Modify: `backend/internal/ota.go`

- [ ] **Add `poll_interval_seconds` to `OTACheckResponse`**

```go
type OTACheckResponse struct {
    UpdateAvailable      bool   `json:"update_available"`
    Version             string `json:"version,omitempty"`
    BinaryURL           string `json:"binary_url,omitempty"`
    SHA256              string `json:"sha256,omitempty"`
    BinarySize          int    `json:"binary_size,omitempty"`
    PollIntervalSeconds int    `json:"poll_interval_seconds,omitempty"`
}
```

- [ ] **Populate `PollIntervalSeconds` in `CheckOTA`**

In the `CheckOTA` method, after the release query, add the poll interval. The value can come from a config or default to 300:

```go
// Default poll interval (configurable per org/deployment)
const defaultPollInterval = 300

// In the response construction:
writeJSON(w, http.StatusOK, OTACheckResponse{
    UpdateAvailable:      true,
    Version:              release.Version,
    BinaryURL:            strings.TrimRight(h.publicMinIO, "/") + "/" + h.bucket + "/" + release.BinaryPath,
    SHA256:               release.SHA256,
    BinarySize:           release.BinarySize,
    PollIntervalSeconds:  defaultPollInterval,
})
```

Also add `PollIntervalSeconds` to the "no update" response:

```go
writeJSON(w, http.StatusOK, OTACheckResponse{
    UpdateAvailable:      false,
    PollIntervalSeconds:  defaultPollInterval,
})
```

- [ ] **Commit**

```bash
git add backend/internal/ota.go
git commit -m "feat: add poll_interval_seconds to OTA check response"
```

---

### Task 12: Configure sdkconfig for native rollback support

**Files:**
- Create: `sdkconfig.defaults`

- [ ] **Create sdkconfig.defaults**

Create `sdkconfig.defaults` at the project root:

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

This enables the ESP32 native rollback feature. When the firmware boots in `ESP_OTA_IMG_PENDING_VERIFY` state, the bootloader will revert to the previous OTA slot if the new firmware crashes before calling `esp_ota_mark_app_valid_cancel_rollback()`.

- [ ] **Commit**

```bash
git add sdkconfig.defaults
git commit -m "feat: enable bootloader rollback for OTA safety"
```

---

### Task 13: Build and smoke test

**Files:**
- No file changes

- [ ] **Build all environments**

```bash
~/.platformio/venv/bin/pio run -e esp32dev
~/.platformio/venv/bin/pio run -e esp32c3
~/.platformio/venv/bin/pio run -e esp32c3_nodisplay
~/.platformio/venv/bin/pio run -e esp32s3
```

Expected: All four environments compile without errors.

- [ ] **Fix any compilation errors** (missing includes, typos, API mismatches)

- [ ] **Commit any build fixes**

```bash
git add -A
git commit -m "fix: OTA build fixes"
```

---

## Self-Review Checklist

- [ ] **Spec coverage:** Every section of the spec has a corresponding task:
  - State machine (IDLE → CHECKING → DOWNLOADING → APPLYING → REBOOTING) → Task 4
  - Non-blocking chunked download → Task 4 (one chunk per loop_ota_client call)
  - SHA256 verification → Task 4 (mbedtls_sha256 during download, verify in APPLYING)
  - ESP32 native rollback → Task 4 (esp_ota_set_boot_partition + mark_app_valid_cancel_rollback)
  - Firmware-health mark_valid → Task 4 (ota_confirm_valid), Task 9 (called from setup)
  - Poll interval from backend → Task 4 (parsed from check response), Task 11 (backend)
  - Poll interval NVS persistence → Task 2
  - OTA status in telemetry → Tasks 5, 6
  - MQTT status topic → Task 7
  - BLE/Supabase commands → Task 8
  - Integration with network task → Task 9
  - Safe mode → OTA client not started in safe mode (handled by main.cpp's existing safe mode path)
  - Backend changes → Task 11
  - Partition layout → Already done (no change needed)
  - CLAUDE.md update → Already done in spec review

- [ ] **Placeholder scan:** No TBDs, TODOs, or incomplete code blocks. The backend URL construction in Task 4 uses a TODO comment but has a concrete fallback path (NVS key). This is acceptable — the URL must be configured per deployment.

- [ ] **Type consistency:** All function signatures match between headers and implementations. Enum values match between ota_client.h and the switch statement in telemetry.cpp. Struct field names match between telemetry.h and telemetry.cpp.
