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
            // Read backend URL from NVS (set via BLE/Supabase ota_set_backend_url)
            char backend_url[128] = "";
            if (!settings_load_ota_backend_url(backend_url, sizeof(backend_url))) {
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

    // Wait for the grace period before confirming. This gives the firmware
    // time to complete init, run a few sensor ticks, and prove it's stable.
    // If the device crashes during the grace window, the bootloader reverts.
    if (millis() < (unsigned long)OTA_GRACE_SECONDS * 1000) {
        return; // grace period not yet elapsed
    }

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
