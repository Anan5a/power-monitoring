#ifndef CONFIG_H
#define CONFIG_H

// Board-specific pin/address abstraction.
// Each PlatformIO environment must pass exactly one of:
//   -DBOARD_ESP32DEV, -DBOARD_ESP32C3, or -DBOARD_ESP32S3
#if defined(BOARD_ESP32DEV)
    #include "boards/esp32dev.h"
#elif defined(BOARD_ESP32C3)
    #include "boards/esp32c3.h"
#elif defined(BOARD_ESP32S3)
    #include "boards/esp32s3.h"
#else
    #warning "No BOARD_* build flag defined; falling back to esp32c3 board pinout. Add -DBOARD_ESP32DEV/C3/S3 to your PlatformIO build_flags."
    #include "boards/esp32c3.h"
#endif

// WiFi credentials
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// MQTT broker settings
#define MQTT_BROKER   "192.168.1.100"
#define MQTT_PORT     1883
#define MQTT_TOPIC        "power-monitor/data"
#define MQTT_LOG_TOPIC    "power-monitor/logbin"

// Device type used in the MQTT telemetry topic `telemetry/{device_type}/{device_key}`
// and matched against OTA releases in the backend. Override per product via
// build_flags. The device_key segment is read from NVS (Supabase device_key).
#ifndef DEVICE_TYPE
#define DEVICE_TYPE "power_monitor_v2"
#endif

// Blynk IoT (Blynk 2.0 requires TEMPLATE_ID and TEMPLATE_NAME)
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxx"
#define BLYNK_TEMPLATE_NAME "PowerMonitor"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_TOKEN"

// Hardware enable/disable
#define ENABLE_INA3221         0   // legacy 3-channel current/voltage module (kept for migration only)
#define ENABLE_INA3221_VOLT    0   // legacy voltage module
#define ENABLE_INA226          1   // primary DC measurement (multiple modules via I2C)
#define ENABLE_ADS1115        0
#ifndef HAS_DISPLAY
#define HAS_DISPLAY           1   // SSD1306 OLED on I2C
#endif
// Debug Serial console. Set to 0 for builds that disable the USB-CDC console
// (e.g. esp32c3_nodisplay with ARDUINO_USB_CDC_ON_BOOT=1) where `Serial` is
// not declared in scope. When 0, every LOG_PRINT/LOG_PRINTLN call in
// include/log_serial.h becomes a no-op.
#ifndef HAS_SERIAL
#define HAS_SERIAL            1
#endif

// Maximum number of sensor pods supported at compile time
#define MAX_INA226          8
#define MAX_BL0939          4
#define MAX_BL0939_CHANNELS (MAX_BL0939 * 2)

// === Voltage divider ratios (hardware fixed) ===
// ratio = (R_high + R_low) / R_low
#define VOLT_RATIO_CH0   3.5210f   // battery bank: 300k+119k/119k → 48V max
#define VOLT_RATIO_CH1  14.2353f   // solar PV: 900k+68k/68k → 200V max
#define VOLT_RATIO_CH2  14.2353f   // output/load: 900k+68k/68k → 200V max

// === Default calibration (user can override via UI/BLE) ===
#define CAL_VOLT_OFFSET_MV_CH0  0.0f
#define CAL_VOLT_OFFSET_MV_CH1  0.0f
#define CAL_VOLT_OFFSET_MV_CH2  0.0f
#define CAL_VOLT_GAIN_CH0       1.0f
#define CAL_VOLT_GAIN_CH1       1.0f
#define CAL_VOLT_GAIN_CH2       1.0f
#define CAL_CURR_OFFSET_MA_CH0  0.0f
#define CAL_CURR_OFFSET_MA_CH1  0.0f
#define CAL_CURR_OFFSET_MA_CH2  0.0f
#define CAL_CURR_GAIN_CH0       1.0f
#define CAL_CURR_GAIN_CH1       1.0f
#define CAL_CURR_GAIN_CH2       1.0f

// I2C device addresses
#define INA3221_ADDR    0x40
#define INA226_ADDR     0x41
#define ADS1115_ADDR    0x48
#define OLED_ADDR       0x3C

// Sampling and display timing
// LEGACY — superseded by FreeRTOS task timing in src/main.cpp. These
// constants are kept because other modules (and the serial CLI) still
// reference them as display/intervals. New timing should be set via the
// FreeRTOS task periods in main.cpp.
#define SAMPLE_INTERVAL_MS  5000
#define FAST_SAMPLE_INTERVAL_MS 500
#define DISPLAY_INTERVAL_MS 1000

// Baseline noise calibration / spike detection (disable if noisy hardware causes false spikes)
#define ENABLE_BASELINE_CALIBRATION 0

// Serial / BL0939 energy-meter interface (pins defined in the selected board header)
#define ENABLE_SERIAL1       0   // set to 1 to enable legacy Serial1 reader
#define SERIAL1_BAUD         9600

#define ENABLE_BL0939        0   // set to 1 when a BL0939 UART meter is wired to BL0939_*_PIN

// Protobuf encoding (nanopb). Default 0 = JSON. Set to 1 to encode telemetry
// as protobuf and publish to telemetry/{type}/{key}/pb (MQTT topic suffix).
// The backend does not yet consume the /pb topic — this is firmware-side
// plumbing only. See docs/FIRMWARE_PLAN.md for the negotiation gap.
#ifndef USE_PROTOBUF
#define USE_PROTOBUF         0
#endif

// BLE settings
#define BT_DEVICE_NAME          "PowerMonitor"
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Data logging
#define LOG_BUFFER_BYTES      (16 * 1024)
#define LOG_OVERFLOW_FILE     "/log_overflow.bin"
#define LOG_MAX_DELTA_MV      32760
#define LOG_MAX_DELTA_MA      32760
#define LOG_MAX_DELTA_POWER   32760

// MQTT — PubSubClient reads MQTT_MAX_PACKET_SIZE at include time, so the
// guard lets the user override from build_flags without us stomping on it.
// Bumped from the library default of 256 to 2048 so JSON telemetry payloads
// (with metadata + 4 channel rows ≈ 1.4 KB) fit without truncation.
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 4096
#endif

// BLE command interface UUIDs
#define BLE_CHAR_CMD_UUID       "c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4"
#define BLE_CHAR_RESP_UUID      "d8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01"
#define BLE_CHAR_STATUS_UUID    "e3c5a7f2-8b1d-4e6c-9a0f-2d4b6e8c1a35"

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

#endif