#ifndef BOARDS_ESP32S3_H
#define BOARDS_ESP32S3_H

// ESP32-S3-DevKitC-1 pin defaults.
// SPI0/SPI1 flash pins (26-32) and PSRAM pins (33/34 on most modules) are
// avoided. Native USB pins 19-21 are avoided. Strapping pins 0, 3, 45, 46
// are avoided for outputs.

// Highest legal GPIO number on this board. switch_controller.cpp uses this
// to reject out-of-range pins in NVS before pinMode() is called.
#define BOARD_GPIO_MAX 48

// I2C bus pins
#define I2C_SDA         8
#define I2C_SCL         9
#define I2C_FREQ        100000UL

// Status/user feedback
#define STATUS_LED_GPIO 48  // RGB LED on ESP32-S3-DevKitC-1
#define USER_BUTTON_GPIO 0 // BOOT button, has external pull-up

// UI buttons and indicator LEDs
#define UI_BUTTON_COUNT 4
#define UI_BUTTON_PINS  { 0, -1, -1, -1 }
#define UI_LED_NETWORK_GPIO 48
#define UI_LED_ERROR_GPIO   -1
#define UI_LED_OK_GPIO      -1

// Relay / MOSFET driver GPIO defaults
#define RELAY_1_GPIO    10
#define RELAY_2_GPIO    11
#define RELAY_3_GPIO    12
#define RELAY_4_GPIO    13

// BL0939 energy-meter UART defaults (Serial1 / UART2 on ESP32-S3)
#define BL0939_UART_NUM  2
#define BL0939_RX_PIN    15
#define BL0939_TX_PIN    16

// Legacy Serial1 reader defaults (shared with BL0939 UART)
#define SERIAL1_RX_PIN   BL0939_RX_PIN
#define SERIAL1_TX_PIN   BL0939_TX_PIN

// INA226 DC sensor defaults
#define INA226_COUNT        4
#define INA226_ADDRESSES    {0x40, 0x41, 0x42, 0x43}
#define INA226_SHUNTS       {0.005f, 0.005f, 0.005f, 0.005f}
#define INA226_VOLT_RATIOS  {1.0f, 1.0f, 1.0f, 1.0f}
#define INA226_I_GAINS      {1.0f, 1.0f, 1.0f, 1.0f}
#define INA226_V_OFFSETS    {0.0f, 0.0f, 0.0f, 0.0f}

// BL0939 AC energy-meter defaults
#define BL0939_COUNT        0
#define BL0939_ADDRESSES    {0}
#define BL0939_BAUD         4800

#endif // BOARDS_ESP32S3_H
