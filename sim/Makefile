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

.PHONY: all clean run test_cycle_counter

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
    ../src/cycle_counter.cpp \
    ../src/coulomb_counter.cpp \
    ../src/settings_manager.cpp \
    ../src/battery_state.cpp \
    ../src/battery_profile.cpp \
    ../src/battery_nvs.cpp

TEST_CC_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/test_cc_%.o,$(notdir $(TEST_CC_SRCS)))
TEST_CC_TARGET = $(BUILD_DIR)/test_cycle_counter

test_cycle_counter: $(TEST_CC_TARGET)
	./$(TEST_CC_TARGET)

$(TEST_CC_TARGET): $(TEST_CC_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/test_cc_%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
