cmake_minimum_required(VERSION 3.16)
project(power_monitor_sim)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(HAS_DISPLAY=0)
add_compile_options(-Wall -Wextra -Wno-format -Wno-missing-field-initializers)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/mock
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

set(SIM_SOURCES
    main.cpp
    shims/sensor_manager.cpp
    shims/connectivity_manager.cpp
    hal/arduino_stubs.cpp
    hal/preferences_stub.cpp
    hal/wifi_stub.cpp
    hal/littlefs_stub.cpp
    hal/wire_stub.cpp
    hal/gpio_stub.cpp
    mock/waveform_generator.cpp
    mock/ina226_mock.cpp
    mock/bl0939_mock.cpp
)

set(FW_SOURCES
    ../src/data_logger.cpp
    ../src/coulomb_counter.cpp
    ../src/energy_counter.cpp
    ../src/settings_manager.cpp
)

add_executable(power_monitor_sim ${SIM_SOURCES} ${FW_SOURCES})
