# UI Manager

The UI manager provides button debouncing, LED status indicators, and display page cycling with minimal GPIO usage.

## Hardware Configuration

Configure in your board header (`include/boards/*.h`):

```c
// Buttons
#define UI_BUTTON_COUNT 4
#define UI_BUTTON_PINS  {0, -1, -1, -1}   // GPIO 0 = BOOT button

// LEDs (-1 = not connected)
#define UI_LED_NETWORK_GPIO 2    // Network/cloud status
#define UI_LED_ERROR_GPIO   -1   // Fault indicator (unmapped by default)
#define UI_LED_OK_GPIO      -1   // Heartbeat (unmapped by default)
```

## Button Events

The UI manager debounces buttons with a 20ms threshold and detects:

| Event | Trigger | Default Action |
|---|---|---|
| Short press | 30ms–5s hold | Button 0: cycle display page |
| | | Button 1: toggle switch 0 |
| | | Button 2: start baseline calibration |
| Long press | ≥5s hold | Button 3: factory reset + reboot |
| Double press | Two presses <300ms apart | Reserved for future use |

## LED Patterns

| Pattern | Behavior |
|---|---|
| `LED_OFF` | Always off |
| `LED_ON` | Always on |
| `LED_SLOW_BLINK` | 500ms toggle (1 Hz) |
| `LED_FAST_BLINK` | 125ms toggle (4 Hz) |
| `LED_PULSE` | 250ms on/off (2 Hz) |

## Status Indicators

The network task feeds status into the UI manager:

```cpp
// Called from networkTask when WiFi/cloud state changes
ui_set_network_status(wifi_connected, cloud_connected);
```

This automatically sets the network LED:

| State | LED Pattern |
|---|---|
| No WiFi | Slow blink |
| WiFi, no cloud | Fast blink |
| WiFi + cloud | Solid on |

## Display Page Cycling

Button 0 (short press) triggers `ui_next_display_page()` to return `true` once. The display update in `networkTask` can check this to advance the OLED page. Currently the page cycling is a hook; the display manager's page logic is unchanged.

## Adding a New Button Action

In `src/ui_manager.cpp`, `handle_button_event()`:

```cpp
if (short_press) {
    switch (idx) {
        case 0: next_display_page = 1; break;
        case 1: switch_set(0, !get_switch_state(0)); break;
        case 2: sensor_calibrate_baseline(); break;
        default: break;
    }
}
if (long_press && idx == 3) {
    // factory reset
}
```

## FreeRTOS Task

The UI manager runs in its own task (`uiTask`) on Core 1 at 50ms:

```cpp
xTaskCreatePinnedToCore(uiTask, "UI", 2048, NULL, 2, NULL, 1);
```

This ensures button debounce and LED updates are independent of the 1-second sensor loop and the 10ms network loop.
