#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

enum LedPattern { LED_OFF = 0, LED_ON, LED_SLOW_BLINK, LED_FAST_BLINK, LED_PULSE };

void init_ui();
void loop_ui();

void ui_set_led(uint8_t led, LedPattern pattern);
void ui_set_network_status(bool wifi_connected, bool cloud_connected);
void ui_set_fault(bool fault);
void ui_set_heartbeat(bool healthy);

// Returns true once after a display-page-cycle button event.
bool ui_next_display_page();

#endif
