#ifndef CONFIG_H
#define CONFIG_H

// WiFi credentials
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// MQTT broker settings
#define MQTT_BROKER   "192.168.1.100"
#define MQTT_PORT     1883
#define MQTT_TOPIC        "power-monitor/data"
#define MQTT_LOG_TOPIC    "power-monitor/logbin"

// Blynk IoT (Blynk 2.0 requires TEMPLATE_ID and TEMPLATE_NAME)
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxx"
#define BLYNK_TEMPLATE_NAME "PowerMonitor"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_TOKEN"

// I2C bus pins (ESP32-C3)
#define I2C_SDA         5
#define I2C_SCL         6
#define I2C_FREQ        100000

// Hardware enable/disable
#define ENABLE_INA3221         1   // current module 0x40
#define ENABLE_INA3221_VOLT    1   // voltage module 0x42
#define ENABLE_INA226          0
#define ENABLE_ADS1115        0
#ifndef HAS_DISPLAY
#define HAS_DISPLAY           1   // SSD1306 OLED on I2C
#endif

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
#define CAL_CURR_OFFSET_MA_CH2  12.0f   // ghost current on CH2
#define CAL_CURR_GAIN_CH0       1.0f
#define CAL_CURR_GAIN_CH1       1.0f
#define CAL_CURR_GAIN_CH2       1.0f

// I2C device addresses
#define INA3221_ADDR    0x40
#define INA226_ADDR     0x41
#define ADS1115_ADDR    0x48
#define OLED_ADDR       0x3C

// Sampling and display timing
#define SAMPLE_INTERVAL_MS  5000
#define FAST_SAMPLE_INTERVAL_MS 500
#define DISPLAY_INTERVAL_MS 1000

// Baseline noise calibration / spike detection (disable if noisy hardware causes false spikes)
#define ENABLE_BASELINE_CALIBRATION 0

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

// Relay GPIO defaults (ESP32-C3)
#define RELAY_1_GPIO    7
#define RELAY_2_GPIO    10
#define RELAY_3_GPIO    20
#define RELAY_4_GPIO    21

// Serial1 interface (external device: inverter, generator, etc.)
// TX not needed — RX only on D23
#define ENABLE_SERIAL1       0   // set to 1 to enable Serial1 reader
#define SERIAL1_RX_PIN       23
#define SERIAL1_BAUD         9600
#define SERIAL1_BUFFER       256  // ring buffer size in bytes

// BLE command interface UUIDs
#define BLE_CHAR_CMD_UUID       "c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4"
#define BLE_CHAR_RESP_UUID      "d8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01"
#define BLE_CHAR_STATUS_UUID    "e3c5a7f2-8b1d-4e6c-9a0f-2d4b6e8c1a35"

#endif