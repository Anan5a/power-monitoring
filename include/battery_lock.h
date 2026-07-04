#ifndef BATTERY_LOCK_H
#define BATTERY_LOCK_H

// battery_lock.h
// =============================================================================
// Shared critical-section guard for the per-channel battery state caches.
//
// The cycle counter (g_state[] / g_loaded[] / g_last_dir[] in
// cycle_counter.cpp), the chemistry profile table (g_profiles[] / g_loaded in
// battery_profile.cpp), and the channel→profile binding table
// (g_channel_profile[] in battery_state.cpp) are written from sensorTask at
// 1Hz and read from networkTask at 5s. Without a critical section the network
// task can observe a partially updated struct (e.g. cumulative_Ah_in from
// after the direction flip but cumulative_Ah_out from before).
//
// Lock discipline:
//   - Lock windows MUST stay tight. No NVS, no Serial, no I/O under the lock.
//   - All BatteryState / BatteryChemistryProfile reads from the network task
//     use cycle_counter_snapshot() to copy the full struct under one lock.
//   - Per-field getters take/release the lock around a single read; they are
//     safe but the snapshot helper is preferred for point-in-time consistency.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

#if defined(ESP32) || defined(ESP32C3) || defined(ESP32S3)
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>

    // FreeRTOS-style portMUX for cross-core critical sections. Initialise via
    // battery_lock_init() once during init_core_shared() (or analogous).
    extern portMUX_TYPE g_battery_mux;

    #define BATTERY_LOCK_INIT()   battery_lock_init()
    #define BATTERY_LOCK()        taskENTER_CRITICAL(&g_battery_mux)
    #define BATTERY_UNLOCK()      taskEXIT_CRITICAL(&g_battery_mux)
#else
    // Host build (sim unit tests): no FreeRTOS, no critical section.
    // The tests are single-threaded, so a no-op is correct.
    #define BATTERY_LOCK_INIT()   do {} while (0)
    #define BATTERY_LOCK()        do {} while (0)
    #define BATTERY_UNLOCK()      do {} while (0)
#endif

void battery_lock_init();

#endif // BATTERY_LOCK_H
