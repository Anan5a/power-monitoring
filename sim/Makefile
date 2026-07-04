CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wno-format -Wno-missing-field-initializers -Ishims -Ihal -Imock -I../include -DHAS_DISPLAY=0 -DBOARD_ESP32C3=1
BUILD_DIR ?= build
TARGET ?= $(BUILD_DIR)/power_monitor_sim

SRCS = \
    main.cpp \
    shims/sensor_manager.cpp \
    shims/connectivity_manager.cpp \
    hal/arduino_stubs.cpp \
    hal/preferences_stub.cpp \
    hal/wifi_stub.cpp \
    hal/littlefs_stub.cpp \
    hal/wire_stub.cpp \
    hal/gpio_stub.cpp \
    mock/waveform_generator.cpp \
    mock/ina226_mock.cpp \
    mock/bl0939_mock.cpp \
    ../src/data_logger.cpp \
    ../src/coulomb_counter.cpp \
    ../src/energy_counter.cpp \
    ../src/settings_manager.cpp

OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(notdir $(SRCS)))

vpath %.cpp .:shims:hal:mock:../src

.PHONY: all clean run test_cycle_counter test_bl0939_crc test_publish_path test_data_logger

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

# ─────────────────────────────────────────────────────────────────────────────
# Unit tests
# ─────────────────────────────────────────────────────────────────────────────
# test_cycle_counter: host-side test for the DoD-weighted cycle counter.
# Compiles the real cycle_counter.cpp, battery_state.cpp, battery_profile.cpp,
# coulomb_counter.cpp, and settings_manager.cpp against the existing HAL
# stubs. Drives scenarios through the public update_cycle_counter() API
# and asserts equivalent_full_cycles.

TEST_CC_SRCS = \
    test_cycle_counter.cpp \
    shims/sensor_manager.cpp \
    hal/arduino_stubs.cpp \
    hal/preferences_stub.cpp \
    hal/wifi_stub.cpp \
    hal/littlefs_stub.cpp \
    hal/wire_stub.cpp \
    hal/gpio_stub.cpp \
    hal/test_stubs.cpp \
    ../src/cycle_counter.cpp \
    ../src/coulomb_counter.cpp \
    ../src/settings_manager.cpp \
    ../src/battery_state.cpp \
    ../src/battery_profile.cpp \
    ../src/battery_nvs.cpp \
    ../src/capacity_test.cpp

TEST_CC_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/test_cc_%.o,$(notdir $(TEST_CC_SRCS)))
TEST_CC_TARGET = $(BUILD_DIR)/test_cycle_counter

test_cycle_counter: $(TEST_CC_TARGET)
	./$(TEST_CC_TARGET)

$(TEST_CC_TARGET): $(TEST_CC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test_cc_%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────
# test_bl0939_crc: host-side test for the BL0939 frame parser / CRC.
# Compiles the real bl0939_pod.cpp with BL0939_EXPOSE_FOR_TESTING=1, which
# links the static parse_frame and slot_for_address through the
# *_for_test entry points. ENABLE_BL0939 is left at 0 (the default in
# config.h) so the UART-drain path is excluded — only the pure parser
# helpers are exercised. Tests cover (1) checksum round-trip, (2)
# corrupted-checksum rejection, (3) unknown-address rejection.
# ─────────────────────────────────────────────────────────────────────────────

TEST_B0939_SRCS = \
    test_bl0939_crc.cpp \
    hal/arduino_stubs.cpp \
    hal/preferences_stub.cpp \
    hal/wifi_stub.cpp \
    hal/littlefs_stub.cpp \
    hal/wire_stub.cpp \
    hal/gpio_stub.cpp \
    ../src/bl0939_pod.cpp

TEST_B0939_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/test_b0939_%.o,$(notdir $(TEST_B0939_SRCS)))
TEST_B0939_TARGET = $(BUILD_DIR)/test_bl0939_crc

test_bl0939_crc: CXXFLAGS += -DBL0939_EXPOSE_FOR_TESTING=1
test_bl0939_crc: $(TEST_B0939_TARGET)
	./$(TEST_B0939_TARGET)

$(TEST_B0939_TARGET): $(TEST_B0939_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test_b0939_%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────
# test_data_logger: host-side test for the data logger API.
# Compiles the real data_logger.cpp against the existing HAL stubs.
# Exercises the occupancy/drain APIs added by the B1/B2 audit fix:
#   - log_buffer_used_pct() returns 0 when empty and > 0 after writes
#   - log_buffer_capacity() matches LOG_BUFFER_BYTES
#   - log_entries_count() advances by 1 per log_sample
#   - log_pop_batch() drains the buffer; entries count drops to 0
# The cross-core critical section (B2) is a no-op on the host; the
# firmware target is the real coverage.
# ─────────────────────────────────────────────────────────────────────────────

TEST_DL_SRCS = \
    test_data_logger.cpp \
    shims/sensor_manager.cpp \
    hal/arduino_stubs.cpp \
    hal/preferences_stub.cpp \
    hal/wifi_stub.cpp \
    hal/littlefs_stub.cpp \
    hal/wire_stub.cpp \
    hal/gpio_stub.cpp \
    ../src/data_logger.cpp \
    ../src/coulomb_counter.cpp \
    ../src/settings_manager.cpp

TEST_DL_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/test_dl_%.o,$(notdir $(TEST_DL_SRCS)))
TEST_DL_TARGET = $(BUILD_DIR)/test_data_logger

test_data_logger: $(TEST_DL_TARGET)
	./$(TEST_DL_TARGET)

$(TEST_DL_TARGET): $(TEST_DL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test_dl_%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────
# test_publish_path: host-side validation harness for the firmware's publish
# path. Catches the class of bugs where the firmware compiles clean but
# breaks against a real Supabase schema or a real PubSubClient.
#
# Compiles the real telemetry.cpp, battery_profile.cpp, battery_state.cpp,
# coulomb_counter.cpp, cycle_counter.cpp, capacity_test.cpp, data_logger.cpp,
# and settings_manager.cpp against the HAL stubs. The test re-implements
# the small RPC-envelope builders (mirroring connectivity_manager.cpp) so
# the actual Supabase request shape can be inspected via the http_stub
# HTTPClient mock. Coverage:
#   1. TelemetrySnapshot v1 JSON shape
#   2. sync_battery_profiles / sync_battery_bindings payload
#   3. claim_settings_command payload
#   4. MQTT LWT connect() overload (source-grep)
#   5. set_relay / set_switch payload shape
#   6. NaN/Inf downgrade
# ─────────────────────────────────────────────────────────────────────────────

ARDUINOJSON_PATH ?= ../.pio/libdeps/esp32c3/ArduinoJson/src

TEST_PP_CXXFLAGS = $(CXXFLAGS) -I$(ARDUINOJSON_PATH) -include hal/ESP.h

TEST_PP_SRCS = \
    test_publish_path.cpp \
    shims/sensor_manager.cpp \
    shims/connectivity_manager.cpp \
    shims/connectivity_publish_shim.cpp \
    hal/arduino_stubs.cpp \
    hal/preferences_stub.cpp \
    hal/wifi_stub.cpp \
    hal/littlefs_stub.cpp \
    hal/wire_stub.cpp \
    hal/gpio_stub.cpp \
    hal/http_stub.cpp \
    hal/esp_stub.cpp \
    ../src/telemetry.cpp \
    ../src/cycle_counter.cpp \
    ../src/coulomb_counter.cpp \
    ../src/energy_counter.cpp \
    ../src/settings_manager.cpp \
    ../src/battery_state.cpp \
    ../src/battery_profile.cpp \
    ../src/battery_nvs.cpp \
    ../src/capacity_test.cpp \
    ../src/data_logger.cpp

TEST_PP_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/test_pp_%.o,$(notdir $(TEST_PP_SRCS)))
TEST_PP_TARGET = $(BUILD_DIR)/test_publish_path

test_publish_path: $(TEST_PP_TARGET)
	./$(TEST_PP_TARGET)

$(TEST_PP_TARGET): $(TEST_PP_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_PP_CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test_pp_%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(TEST_PP_CXXFLAGS) -c -o $@ $<
