CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wno-format -Wno-missing-field-initializers -Ishims -Ihal -Imock -I../include -DHAS_DISPLAY=0
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

.PHONY: all clean run

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
