# Sensor Configuration

## Pod/Channel Model

The firmware uses a **pod/channel model** to abstract physical sensors:

- A **Pod** is a physical measurement unit (one INA226 module, one BL0939 chip).
- A **Logical Channel** is a flat index across all pods, used by logging, counters, and cloud APIs.
- Pods are registered at boot in `init_sensors()` via `register_pod()`.

## INA226 — DC Voltage/Current/Power

### Hardware Setup

INA226 modules communicate over I2C. Each module has a unique address (A0/A1 pins):

| A1 | A0 | Address |
|---|---|---|
| GND | GND | 0x40 |
| GND | VS | 0x41 |
| GND | SDA | 0x42 |
| GND | SCL | 0x43 |
| VS | GND | 0x44 |
| VS | VS | 0x45 |
| VS | SDA | 0x46 |
| VS | SCL | 0x47 |
| SDA | GND | 0x48 |
| SDA | VS | 0x49 |
| SDA | SDA | 0x4A |
| SDA | SCL | 0x4B |
| SCL | GND | 0x4C |
| SCL | VS | 0x4D |
| SCL | SDA | 0x4E |
| SCL | SCL | 0x4F |

Up to 16 modules can share one I2C bus without a multiplexer.

### Configuration

In your board header (`include/boards/esp32s3.h` for example):

```c
#define INA226_COUNT        4
#define INA226_ADDRESSES    {0x40, 0x41, 0x42, 0x43}
#define INA226_SHUNTS       {0.005f, 0.005f, 0.005f, 0.005f}
#define INA226_VOLT_RATIOS  {1.0f, 1.0f, 1.0f, 1.0f}
#define INA226_I_GAINS      {1.0f, 1.0f, 1.0f, 1.0f}
#define INA226_V_OFFSETS    {0.0f, 0.0f, 0.0f, 0.0f}
```

- `INA226_COUNT`: Number of modules to initialize (up to `MAX_INA226=8`).
- `INA226_ADDRESSES`: I2C address for each module.
- `INA226_SHUNTS`: Shunt resistor value in ohms for each module.
- `INA226_VOLT_RATIOS`: Voltage divider ratio `(R_high + R_low) / R_low` for each module.
- `INA226_I_GAINS`: Current calibration multiplier (default 1.0).
- `INA226_V_OFFSETS`: Voltage offset in volts (default 0.0).

Shunt values can be overridden at runtime via NVS using `settings_save_shunt(3 + i, ohms)`.

### How It Works

Each INA226 module is registered as a single-channel pod. The pod read function:

1. Reads bus voltage via `getBusVoltage()`.
2. Reads shunt current via `getCurrent()`.
3. Reads power via `getPower()`.
4. Applies voltage ratio, voltage offset, and current gain.
5. Stores the result in `PhysicalChannel.voltage`, `.current`, `.power`.

### Calibration

The INA226 library's `setMaxCurrentShunt()` is called with the configured shunt value. The max current is computed as `0.08192 / shunt_ohms`. This can be overridden at runtime via NVS.

## BL0939 — AC Energy Meter (Stub)

### Hardware Setup

BL0939 chips communicate over UART. Multiple units can share one UART bus if each has a unique address byte.

### Configuration

```c
#define ENABLE_BL0939       1
#define BL0939_COUNT        1
#define BL0939_ADDRESSES    {0x01}
#define BL0939_BAUD         4800
```

UART pins are defined per board in `include/boards/*.h`:

```c
#define BL0939_UART_NUM  2
#define BL0939_RX_PIN    15
#define BL0939_TX_PIN    16
```

### Frame Format

The BL0939 driver parses 24-byte UART frames:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Device address |
| 1-3 | 3 | V_RMS (24-bit unsigned) |
| 4-6 | 3 | I_A_RMS (24-bit signed) |
| 7-9 | 3 | I_B_RMS (24-bit signed) |
| 10-12 | 3 | P_A (24-bit signed) |
| 13-15 | 3 | P_B (24-bit signed) |
| 16-18 | 3 | S_A apparent power (24-bit unsigned) |
| 19-21 | 3 | S_B apparent power (24-bit unsigned) |
| 22 | 1 | Frequency (Hz) |
| 23 | 1 | Checksum |

Each BL0939 provides 2 AC channels (A and B) with shared voltage.

### Status

The BL0939 driver is **compiled but untested on real hardware**. It is disabled by default (`ENABLE_BL0939=0`). Enable it in your board header or `config.h` when you have the hardware wired.

## Legacy INA3221

The old INA3221 code is still present in `sensor_manager.cpp` but **disabled by default** (`ENABLE_INA3221=0`, `ENABLE_INA3221_VOLT=0`). It is kept for migration only and will be removed in a future release.

## Adding a New Sensor Type

1. Define a new `PodType` enum value in `include/sensor_pod.h`.
2. Create a pod read function: `static void pod_my_sensor_read(PodState* pod)`.
3. In `init_sensors()`, initialize the hardware and call `register_pod(POD_MY_TYPE, "name", num_channels, read_fn)`.
4. The pod is automatically included in every `read_sensors()` call.

## Logical Channel Mapping

Logical channels are numbered sequentially in registration order:

```
Pod 0 (INA226 #0): 1 channel  → logical ch 0
Pod 1 (INA226 #1): 1 channel  → logical ch 1
Pod 2 (INA226 #2): 1 channel  → logical ch 2
Pod 3 (INA226 #3): 1 channel  → logical ch 3
Pod 4 (BL0939 #0): 2 channels → logical ch 4, 5
```

The `get_channel_voltage(ch)`, `get_channel_current(ch)`, and `get_channel_power(ch)` helpers use this flat index.
