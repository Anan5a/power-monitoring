#include "WiFi.h"

// A fixed test MAC so telemetry_build() can produce a deterministic
// "device.id" field. The format_mac_device_id() helper prints
// "AABBCCDDEEFF" — 12 hex chars.
static const uint8_t kTestMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
static int g_rssi = -65;

int WiFiClass::status() {
    return WL_CONNECTED;
}

int WiFiClass::RSSI() {
    return g_rssi;
}

int WiFiClass::macAddress(uint8_t* mac) {
    if (mac) {
        for (int i = 0; i < 6; i++) mac[i] = kTestMac[i];
    }
    return 0;
}

// Test-only knob to set RSSI for assertions.
void sim_set_rssi(int rssi) { g_rssi = rssi; }

WiFiClass WiFi;
