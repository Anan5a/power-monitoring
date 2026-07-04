#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>

#define WL_CONNECTED 3

class WiFiClass {
public:
    int status();
    int RSSI();
    // Returns 0 on success and fills mac[6]. Real Arduino's macAddress takes
    // either a uint8_t array or no args (returning a String); we use the
    // array form that telemetry.cpp's format_mac_device_id() calls.
    int macAddress(uint8_t* mac);
};

extern WiFiClass WiFi;

#endif
