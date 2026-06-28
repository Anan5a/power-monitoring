#ifndef BOARDS_ESP32DEV_H
#define BOARDS_ESP32DEV_H

// Generic ESP32 (xtensa) dev board pin defaults.
// Flash/SPI0 are internal pins 6-11 and are not broken out on most dev boards,
// so the pins below are safe for GPIO/I2C on a standard ESP32-DevKitC/V1.

// I2C bus pins
#define I2C_SDA         21
#define I2C_SCL         22
#define I2C_FREQ        100000UL

// Status/user feedback
#define STATUS_LED_GPIO 2
#define USER_BUTTON_GPIO 0

// UI buttons and indicator LEDs
#define UI_BUTTON_COUNT 4
#define UI_BUTTON_PINS  { 0, -1, -1, -1 }
#define UI_LED_NETWORK_GPIO 2
#define UI_LED_ERROR_GPIO   -1
#define UI_LED_OK_GPIO      -1

// Relay / MOSFET driver GPIO defaults
#define RELAY_1_GPIO    25
#define RELAY_2_GPIO    26
#define RELAY_3_GPIO    27
#define RELAY_4_GPIO    14

// BL0939 energy-meter UART defaults (Serial1 / UART2 on ESP32)
#define BL0939_UART_NUM  2
#define BL0939_RX_PIN    16
#define BL0939_TX_PIN    17

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

#endif // BOARDS_ESP32DEV_H
