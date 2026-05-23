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

// I2C bus pins
#define I2C_SDA         21
#define I2C_SCL         22

// I2C device addresses (must be unique on the bus)
#define INA3221_ADDR    0x40
#define INA226_ADDR     0x41
#define ADS1115_ADDR    0x48
#define OLED_ADDR       0x3C

// Sampling and display timing
#define SAMPLE_INTERVAL_MS  5000
#define DISPLAY_INTERVAL_MS 1000

// BLE settings
#define BT_DEVICE_NAME          "PowerMonitor"
#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Data logging
#define LOG_BUFFER_BYTES      (32 * 1024)
#define LOG_SPIFFS_FILE       "/log_overflow.bin"
#define LOG_MAX_DELTA_MV      32760
#define LOG_MAX_DELTA_MA      32760
#define LOG_MAX_DELTA_MW      32760

// Relay GPIO defaults
#define RELAY_1_GPIO    25
#define RELAY_2_GPIO    26
#define RELAY_3_GPIO    27
#define RELAY_4_GPIO    14

// BLE command interface UUIDs
#define BLE_CHAR_CMD_UUID       "c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4"
#define BLE_CHAR_RESP_UUID      "d8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01"
#define BLE_CHAR_STATUS_UUID    "e3c5a7f2-8b1d-4e6c-9a0f-2d4b6e8c1a35"

#endif
