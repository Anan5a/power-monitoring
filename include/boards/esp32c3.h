#ifndef BOARDS_ESP32C3_H
#define BOARDS_ESP32C3_H

// ESP32-C3 (RISC-V) supermini / dev board pin defaults.
// The C3 exposes GPIOs 0-21. Pins 18/19 are native USB D-/D+ and
// pins 20/21 are USB-Serial/JTAG; they are avoided below.
// Strapping pins 2, 8 and 9 are also avoided for outputs/inputs that
// would pull them at boot. STATUS_LED_GPIO (2) is marked accordingly.

// I2C bus pins
#define I2C_SDA         5
#define I2C_SCL         6
#define I2C_FREQ        100000UL

// Status/user feedback
#define STATUS_LED_GPIO 2   // C3 strapping pin: keep drive low at boot
#define USER_BUTTON_GPIO 0  // BOOT button, has external pull-up

// UI buttons and indicator LEDs (GPIO-minimal: 3 LEDs + 4 buttons)
#define UI_BUTTON_COUNT 4
#define UI_BUTTON_PINS  { 0, -1, -1, -1 }  // only Button 0 mapped by default (BOOT)
#define UI_LED_NETWORK_GPIO 2  // shared with STATUS_LED for now
#define UI_LED_ERROR_GPIO   -1
#define UI_LED_OK_GPIO      -1

// Relay / MOSFET driver GPIO defaults
#define RELAY_1_GPIO    1
#define RELAY_2_GPIO    3
#define RELAY_3_GPIO    4
#define RELAY_4_GPIO    7

// BL0939 energy-meter UART defaults (Serial1 / UART1 on ESP32-C3)
#define BL0939_UART_NUM  1
#define BL0939_RX_PIN    10  // verify on your board; early C3 modules used GPIO10 as VDD_SPI
#define BL0939_TX_PIN    -1  // BL0939 TX-only half-duplex; not connected by default

// Legacy Serial1 reader defaults (shared with BL0939 UART)
#define SERIAL1_RX_PIN   BL0939_RX_PIN
#define SERIAL1_TX_PIN   BL0939_TX_PIN

// INA226 DC sensor defaults (I2C addresses 0x40-0x4F possible per chip)
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

#endif // BOARDS_ESP32C3_H
