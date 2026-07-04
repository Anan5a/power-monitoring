// test_stubs.cpp — host-side stubs for functions the test build pulls in
// transitively (capacity_test.cpp → switch_controller.h + telemetry.h).
// The tests for capacity_test only exercise the refusal-to-start path
// (B15) and the auto-cancel-on-reboot path (B8), neither of which calls
// switch_set or telemetry_publish_capacity_test_soh at runtime. We
// still need the symbols to exist so the linker is happy.
//
// This file is only compiled into the test_cycle_counter target — the
// production firmware links the real switch_controller.cpp and
// telemetry.cpp implementations.
#include "switch_controller.h"
#include "telemetry.h"
#include <stdint.h>

void switch_set(uint8_t /*idx*/, bool /*is_energized*/) {}
void switch_pulse(uint8_t /*idx*/, uint32_t /*duration_ms*/) {}
void switch_set_auto(bool /*enabled*/) {}
void init_switches() {}
void evaluate_switches(const SensorSnapshot& /*snap*/) {}
bool get_switch_state(uint8_t /*idx*/) { return false; }
bool switch_gpio_allowed(int8_t /*pin*/) { return true; }

void telemetry_publish_capacity_test_soh(float /*soh_pct*/) {}
