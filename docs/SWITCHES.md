# Switch Controller

The switch controller replaces the old relay controller with a generic abstraction that supports relays, MOSFETs, SSRs, and expander outputs.

## Data Model

### `SwitchChannel` — Physical Output

```cpp
struct SwitchChannel {
    uint8_t idx;            // 0..N-1
    uint8_t type;           // SwitchType enum
    uint8_t gpio_pin;
    bool active_high;       // true: HIGH = energized
    bool enabled;
    bool is_energized;      // persisted physical state
    char name[24];
};
```

### `SwitchType` — Output Type

| Value | Name | Use Case |
|---|---|---|
| 0 | `SW_RELAY` | Electromechanical relay |
| 1 | `SW_MOSFET_LOW_SIDE` | Low-side N-channel MOSFET |
| 2 | `SW_MOSFET_HIGH_SIDE` | High-side P-channel MOSFET |
| 3 | `SW_SSR` | Solid-state relay |
| 4 | `SW_EXPANDER` | I2C/SPI GPIO expander output |

### `SwitchRule` — Protection Rules

```cpp
struct SwitchRule {
    uint8_t switch_idx;       // which switch this rule controls
    uint8_t channel;          // logical sensor channel for thresholds
    float overcurrent_A;      // trip if current > this (0 = disabled)
    float undervoltage_V;     // trip if voltage < this (0 = disabled)
    float soc_low_pct;        // trip if SoC < this (0 = disabled)
    float soc_high_pct;       // trip if SoC > this (0 = disabled)
    uint16_t trip_delay_ms;   // must exceed this duration to trip
    uint16_t reset_delay_ms;  // must stay below threshold this long to reset
    bool enabled;
};
```

## API

```cpp
// Initialize switches from NVS or create defaults
void init_switches();

// Evaluate protection rules (called from sensorTask every 1s)
void evaluate_switches(const SensorSnapshot& snapshot);

// Manual control
void switch_set(uint8_t idx, bool is_energized);   // latch on/off
void switch_pulse(uint8_t idx, uint32_t duration_ms); // press-and-hold

// State queries
bool get_switch_state(uint8_t idx);

// Auto-trip enable/disable
void switch_set_auto(bool enabled);
```

## Auto-Trip Logic

Auto-trip is **disabled by default** for safety. Enable it via:

- Serial CLI: `switch auto on` or `relay auto on`
- BLE: `set_switch` with `enabled: true`
- Supabase command: `set_switch` with `enabled: true`

When enabled, `evaluate_switches()` runs every 1 second:

1. Loads each switch's `SwitchChannel` and `SwitchRule` from NVS.
2. Reads the logical channel's voltage, current, and SoC from the sensor snapshot.
3. If any threshold is exceeded for longer than `trip_delay_ms`, the switch is energized (tripped).
4. Once the condition clears for longer than `reset_delay_ms`, the switch is de-energized (reset).
5. State changes are published to Supabase via `publish_switch_state()`.

## Serial CLI Commands

```
switch status           — show all switch GPIO/type/state
switch N 0/1            — manual override (0=off, 1=on)
test switch N           — pulse for 3 seconds
test all switches       — pulse each switch 1s in sequence
switch auto on          — enable auto-trip
switch auto off         — disable auto-trip
```

Legacy `relay` commands work as aliases for the first 4 switches.

## BLE Commands

```
set_switch  — configure a switch channel and rule
get_switch  — read a switch channel and rule
```

JSON fields for `set_switch`:

```json
{
  "cmd": "set_switch",
  "pin": "123456",
  "idx": 0,
  "type": 0,
  "gpio_pin": 7,
  "active_high": true,
  "enabled": true,
  "channel": 0,
  "overcurrent_A": 5.0,
  "undervoltage_V": 0.0,
  "soc_low_pct": 0.0,
  "soc_high_pct": 0.0,
  "trip_delay_ms": 1000,
  "reset_delay_ms": 5000,
  "rule_enabled": true
}
```

## NVS Persistence

Switch channels are stored under NVS keys `sw_ch_%d` and rules under `sw_rule_%d`. The switch count is stored as `switch_count`. Legacy relay keys (`relay_%d`, `relay_count`) are no longer used.

## Default Configuration

On first boot (no NVS data), `init_switches()` creates 4 relay-type switches with:

- GPIO pins from `RELAY_1_GPIO`..`RELAY_4_GPIO` (board-specific defaults)
- `active_high = true`
- `overcurrent_A = 5.0`
- `trip_delay_ms = 1000`
- `reset_delay_ms = 5000`
- `enabled = true`

## Adding a New Switch Type

1. Add a new value to `SwitchType` enum in `include/switch_controller.h`.
2. In `switch_set()` and `set_switch_pin()`, handle the new type's GPIO semantics.
3. In `init_switches()`, set the default type for new switches.
4. Update the BLE and Serial CLI help text.
