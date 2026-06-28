#include "WiFi.h"

int WiFiClass::status() {
    return WL_CONNECTED;
}

WiFiClass WiFi;
