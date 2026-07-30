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
