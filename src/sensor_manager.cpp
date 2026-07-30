#include "sensor_manager.h"
#include "settings_manager.h"
#include "config.h"
#include "log_serial.h"
#include "bl0939_pod.h"
#include <Wire.h>

#if ENABLE_INA3221
#include <Adafruit_INA3221.h>
static Adafruit_INA3221 ina3221;
#endif

#if ENABLE_INA3221_VOLT
#include <Adafruit_INA3221.h>
static Adafruit_INA3221 ina3221_volt;
#endif

#if ENABLE_INA226
#include <INA226.h>
static INA226* ina226_devices[MAX_INA226];
static uint8_t ina226_device_for_pod[MAX_INA226];
static const uint8_t  ina226_addresses[MAX_INA226]    = INA226_ADDRESSES;
static const float    ina226_shunts[MAX_INA226]      = INA226_SHUNTS;
static const float    ina226_volt_ratios[MAX_INA226] = INA226_VOLT_RATIOS;
static const float    ina226_i_gains[MAX_INA226]     = INA226_I_GAINS;
static const float    ina226_v_offsets[MAX_INA226]   = INA226_V_OFFSETS;
#endif

#if ENABLE_ADS1115
#include <Adafruit_ADS1X15.h>
static Adafruit_ADS1115 ads1115;
#endif

static bool wire_started = false;

static float volt_ratios[3] = {
    VOLT_RATIO_CH0,
    VOLT_RATIO_CH1,
    VOLT_RATIO_CH2,
};

static ChannelCalibration cal = {
    .volt_offset_mv = {
        CAL_VOLT_OFFSET_MV_CH0, CAL_VOLT_OFFSET_MV_CH1, CAL_VOLT_OFFSET_MV_CH2,
        0,0,0,0,0,0,0,0,0,0,0,0,0
    },
    .volt_gain = {
        CAL_VOLT_GAIN_CH0, CAL_VOLT_GAIN_CH1, CAL_VOLT_GAIN_CH2,
        1,1,1,1,1,1,1,1,1,1,1,1,1
    },
    .curr_offset_ma = {
        CAL_CURR_OFFSET_MA_CH0, CAL_CURR_OFFSET_MA_CH1, CAL_CURR_OFFSET_MA_CH2,
        0,0,0,0,0,0,0,0,0,0,0,0,0
    },
    .curr_gain = {
        CAL_CURR_GAIN_CH0, CAL_CURR_GAIN_CH1, CAL_CURR_GAIN_CH2,
        1,1,1,1,1,1,1,1,1,1,1,1,1
    },
};

// Burst sample metadata — indexed by logical channel
static SampleMeta g_meta[MAX_LOGICAL_CHANNELS] = {};
static float baseline_stddev[MAX_LOGICAL_CHANNELS] = {0};
static uint8_t baseline_count = 0;

// ── Pod registry ───────────────────────────────────────────────────────────────

typedef void (*PodReadFn)(PodState* pod);

struct PodDriver {
    PodState state;
    PodReadFn read_fn;
};

static PodDriver g_pods[MAX_PODS];
static uint8_t g_pod_count = 0;
static uint8_t g_logical_count = 0;

static bool register_pod(PodType type, const char* name, uint8_t num_channels, PodReadFn read_fn) {
    if (g_pod_count >= MAX_PODS || num_channels == 0 || num_channels > MAX_CHANNELS_PER_POD || !read_fn) {
        return false;
    }
    PodState* s = &g_pods[g_pod_count].state;
    s->id = g_pod_count;
    s->type = type;
    strlcpy(s->name, name, sizeof(s->name));
    s->num_channels = num_channels;
    for (uint8_t c = 0; c < num_channels; c++) {
        s->channels[c] = PhysicalChannel{};
        s->channels[c].pod_id = g_pod_count;
        s->channels[c].pod_channel = c;
    }
    g_pods[g_pod_count].read_fn = read_fn;
    g_pod_count++;
    g_logical_count += num_channels;
    return true;
}

// ── Helpers ────────────────────────────────────────────────────────────────────

static float median_of(float arr[], int n) {
    // Bubble sort for n=4 — no heap allocation, deterministic
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                float t = arr[j]; arr[j] = arr[j + 1]; arr[j + 1] = t;
            }
        }
    }
    return arr[n / 2];
}

static float stddev_of(float arr[], int n, float med) {
    float variance = 0;
    for (int i = 0; i < n; i++) {
        float d = arr[i] - med;
        variance += d * d;
    }
    return sqrtf(variance / n);
}

static float max_deviation(float arr[], int n, float med) {
    float m = 0;
    for (int i = 0; i < n; i++) m = fmaxf(m, fabsf(arr[i] - med));
    return m;
}

// ── INA3221 legacy pod driver ──────────────────────────────────────────────────
// Each INA3221 logical channel is registered as a single-channel pod.
// The pod id equals the hardware channel index because these pods are registered first.

#if ENABLE_INA3221 || ENABLE_INA3221_VOLT
static void pod_ina3221_read(PodState* pod) {
    uint8_t hw_ch = pod->id;
    PhysicalChannel* ch = &pod->channels[0];
    float samples[BURST_N];

#if ENABLE_INA3221
    for (int i = 0; i < BURST_N; i++) {
        samples[i] = ina3221.getCurrentAmps(hw_ch) * 1000.0f; // mA
    }
    float med = median_of(samples, BURST_N);
    float sd = stddev_of(samples, BURST_N, med);
    float max_dev = max_deviation(samples, BURST_N, med);
    bool spike = false;
#if ENABLE_BASELINE_CALIBRATION
    spike = baseline_count >= BASELINE_TICKS &&
            sd > SPIKE_STDDEV_MULT * baseline_stddev[hw_ch] &&
            max_dev > SPIKE_DEVIATION_MA;
#endif
    g_meta[hw_ch] = {sd, spike};
    ch->meta = {sd, spike};  // also expose to callers via sensor_get_meta()

    float cal_ma = (med - cal.curr_offset_ma[hw_ch]) * cal.curr_gain[hw_ch];
    if (fabsf(cal_ma) < 5.0f) cal_ma = 0.0f; // dead-zone
    float curr_a = cal_ma / 1000.0f;
    if (cal.invert_curr[hw_ch]) curr_a = -curr_a;
    ch->current = curr_a;
#else
    ch->current = 0.0f;
    g_meta[hw_ch] = {0.0f, false};
    ch->meta = {0.0f, false};
#endif

#if ENABLE_INA3221_VOLT
    for (int i = 0; i < BURST_N; i++) {
        samples[i] = ina3221_volt.getBusVoltage(hw_ch) * 1000.0f; // mV
    }
    float v_med = median_of(samples, BURST_N);
    float cal_mv = (v_med + cal.volt_offset_mv[hw_ch]) * cal.volt_gain[hw_ch];
    ch->voltage = cal_mv / 1000.0f * volt_ratios[hw_ch];
#else
    ch->voltage = 0.0f;
#endif

    ch->power = ch->voltage * ch->current;
}
#endif

// ── INA226 pod driver ──────────────────────────────────────────────────────────

#if ENABLE_INA226
static void pod_ina226_read(PodState* pod) {
    PhysicalChannel* ch = &pod->channels[0];
    uint8_t dev_idx = ina226_device_for_pod[pod->id];
    if (dev_idx >= INA226_COUNT || !ina226_devices[dev_idx]) {
        ch->voltage = ch->current = ch->power = 0.0f;
        return;
    }
    float v = ina226_devices[dev_idx]->getBusVoltage();
    float i_a = ina226_devices[dev_idx]->getCurrent();
    float p = ina226_devices[dev_idx]->getPower();
    ch->voltage = v * ina226_volt_ratios[dev_idx] + ina226_v_offsets[dev_idx];
    ch->current = i_a * ina226_i_gains[dev_idx];
    // Power must be consistent with the calibrated V and I; the device's
    // raw getPower() ignores the per-channel calibration applied above, so
    // recompute it (downstream cross-checks of P == V*I would otherwise drift).
    ch->power = ch->voltage * ch->current;
    // INA226 takes a single sample per tick (no burst), so stddev/spike are
    // not computed here — meta is explicitly "no spike". Burst sampling for
    // INA226 is a future enhancement.
    ch->meta = {0.0f, false};
}
#endif

// ── Pod registry helpers ───────────────────────────────────────────────────────

static void clear_pods() {
    for (uint8_t i = 0; i < g_pod_count; i++) {
        g_pods[i] = PodDriver{};
    }
    g_pod_count = 0;
    g_logical_count = 0;
}

// ── I2C auto-discovery ─────────────────────────────────────────────────────────

#if ENABLE_INA226
static void discover_ina226() {
    // When the legacy INA3221 modules are enabled, reserve the 0x40-0x43
    // range so the scan probe doesn't trash the INA3221's configuration
    // registers with a stray INA226 init attempt. The INA3221 sits at
    // 0x40 (current) and 0x42 (voltage); 0x41 and 0x43 are reserved for
    // future INA3221 expansions. New builds can flip ENABLE_INA3221=0 to
    // open up the full 0x40-0x4F range.
    uint8_t scan_lo = 0x40, scan_hi = 0x4F;
#if ENABLE_INA3221 || ENABLE_INA3221_VOLT
    scan_lo = 0x44;
#endif
    uint8_t found = 0;
    LOG_PRINT("[DISC] Scanning I2C for INA226 (0x%02X-0x%02X)...\n", scan_lo, scan_hi);
    for (uint8_t addr = scan_lo; addr <= scan_hi; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() != 0) continue; // no ACK

        // Device ACKed — try INA226 init
        INA226* dev = new INA226(addr, &Wire);
        if (!dev->begin()) {
            delete dev;
            continue;
        }
        if (!dev->isConnected()) {
            delete dev;
            continue;
        }

        // Confirmed INA226 — configure and register
        float shunt = 0.005f;
        float saved_shunt = 0.0f;
        if (settings_load_shunt(found, &saved_shunt) && saved_shunt > 0.0f) {
            shunt = saved_shunt;
        }
        float max_current = 0.08192f / shunt;
        if (max_current < 0.001f) max_current = 0.001f;
        dev->setMaxCurrentShunt(max_current, shunt);

        ina226_devices[found] = dev;
        ina226_device_for_pod[found] = found;

        // Persist the discovered address so the next boot can skip the scan.
        settings_save_discovered_ina_addr(found, addr);

        char name[16];
        snprintf(name, sizeof(name), "INA226@0x%02X", addr);
        register_pod(POD_INA226, name, 1, pod_ina226_read);
        LOG_PRINT("[DISC] INA226 found at 0x%02X (pod %d)\n", addr, g_pod_count - 1);

        found++;
        // Cap at INA226_COUNT (the ina226_devices[]/read limit). MAX_INA226 is
        // a compile-time ceiling (8) but ina226_devices is only INA226_COUNT
        // entries, so registering more would create pods that always read zero.
        if (found >= INA226_COUNT) break;
    }

    // Persist the final count so init_sensors() can use the cache next boot.
    settings_save_discovered_ina_count(found);
    LOG_PRINT("[DISC] INA226 scan complete: %d found\n", found);
}
#endif

// ── Init ───────────────────────────────────────────────────────────────────────

void init_sensors() {
    if (!wire_started) {
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(I2C_FREQ);
        wire_started = true;
    }

    ChannelCalibration saved;
    if (settings_load_channel_calibration(&saved)) cal = saved;

    for (uint8_t ch = 0; ch < 3; ch++) {
        float ratio = 0.0f;
        if (settings_load_volt_ratio(ch, &ratio) && ratio > 0.0f) {
            volt_ratios[ch] = ratio;
        } else {
            float r_h = 0.0f, r_l = 0.0f;
            if (settings_load_resistors(ch, &r_h, &r_l) && r_h > 0.0f && r_l > 0.0f) {
                volt_ratios[ch] = (r_h + r_l) / r_l;
            }
        }
    }

    // INA226 discovery MUST happen before the INA3221 init. The discovery
    // probe pokes every address in its scan range; if INA3221 has already
    // been begin()'d, the probe can trash its configuration registers
    // (Adafruit_INA3221.reset() is called from begin() too). The scan range
    // is restricted inside discover_ina226() when ENABLE_INA3221 is on,
    // but doing the INA3221 init last is belt-and-braces.
#if ENABLE_INA226
    uint8_t disc_count = settings_load_discovered_ina_count();
    if (disc_count > 0) {
        LOG_PRINT("[DISC] Loading %d INA226 from NVS cache\n", disc_count);
        for (uint8_t i = 0; i < disc_count && i < MAX_INA226; i++) {
            uint8_t addr;
            if (!settings_load_discovered_ina_addr(i, &addr)) continue;
            ina226_devices[i] = new INA226(addr, &Wire);
            if (!ina226_devices[i]->begin()) {
                LOG_PRINT("[DISC] INA226 at 0x%02X init failed (re-scan?)\n", addr);
                delete ina226_devices[i];
                ina226_devices[i] = nullptr;
                continue;
            }
            float shunt = 0.005f;
            float saved_shunt = 0.0f;
            if (settings_load_shunt(i, &saved_shunt) && saved_shunt > 0.0f) {
                shunt = saved_shunt;
            }
            float max_current = 0.08192f / shunt;
            if (max_current < 0.001f) max_current = 0.001f;
            ina226_devices[i]->setMaxCurrentShunt(max_current, shunt);
            ina226_device_for_pod[i] = i;
            char name[16];
            snprintf(name, sizeof(name), "INA226@0x%02X", addr);
            register_pod(POD_INA226, name, 1, pod_ina226_read);
            LOG_PRINT("[DISC] Restored INA226 at 0x%02X (pod %d)\n", addr, g_pod_count - 1);
        }
    } else {
        discover_ina226();
    }
#else
    LOG_PRINTLN("INA226 disabled");
#endif

#if ENABLE_INA3221
    if (!ina3221.begin(INA3221_ADDR, &Wire)) {
        LOG_PRINTLN("INA3221 current (0x40) init failed");
    } else {
        for (uint8_t ch = 0; ch < 3; ch++) {
            float shunt = 0.0f;
            if (settings_load_shunt(ch, &shunt) && shunt > 0.0f) {
                ina3221.setShuntResistance(ch, shunt);
            }
        }
    }
#endif

#if ENABLE_INA3221_VOLT
    if (!ina3221_volt.begin(0x42, &Wire)) {
        LOG_PRINTLN("INA3221 voltage (0x42) init failed");
    }
#endif

#if ENABLE_ADS1115
    if (!ads1115.begin(ADS1115_ADDR, &Wire)) {
        LOG_PRINTLN("ADS1115 init failed");
    } else {
        ads1115.setGain(GAIN_ONE);
    }
#else
    LOG_PRINTLN("ADS1115 disabled");
#endif

    // Register BL0939 AC energy-meter pods (UART). Each BL0939 chip exposes 2
    // channels. The driver was previously implemented but never wired in, so
    // AC readings never flowed. Pods are registered after INA226 so their
    // logical channel indices follow the DC channels.
#if ENABLE_BL0939 && BL0939_COUNT > 0
    bl0939_pod_init();
    {
        const uint8_t bl_count = BL0939_COUNT;
        for (uint8_t i = 0; i < bl_count && i < MAX_BL0939; i++) {
            char name[16];
            snprintf(name, sizeof(name), "BL0939@%u", (unsigned)i);
            register_pod(POD_BL0939, name, 2, bl0939_pod_read);
            // The pod just registered is at g_pod_count-1; bind it to BL0939
            // slot i so bl0939_pod_read() routes the right address's frames.
            bl0939_set_pod_slot((uint8_t)(g_pod_count - 1), i);
            LOG_PRINT("[SENS] registered BL0939 pod %u (slot %u)\n", (unsigned)(g_pod_count - 1), (unsigned)i);
        }
    }
#endif

    // Register legacy INA3221 pods (if enabled and no discovery data)
#if ENABLE_INA3221 || ENABLE_INA3221_VOLT
    if (g_pod_count == 0) {
        for (uint8_t ch = 0; ch < 3; ch++) {
            char name[16];
            snprintf(name, sizeof(name), "CH%d", ch);
            register_pod(POD_INA226, name, 1, pod_ina3221_read);
        }
    }
#endif

    if (g_pod_count == 0) {
        LOG_PRINTLN("[DISC] WARNING: No sensors found! Use 'discover_sensors' CLI/BLE command to re-scan.");
    }
}

void discover_sensors() {
    LOG_PRINTLN("[DISC] Re-discovering sensors...");
    settings_clear_discovered();

    // Delete existing INA226 devices
#if ENABLE_INA226
    for (uint8_t i = 0; i < MAX_INA226; i++) {
        if (ina226_devices[i]) {
            delete ina226_devices[i];
            ina226_devices[i] = nullptr;
        }
    }
#endif

    clear_pods();
    discover_ina226();

    if (g_pod_count == 0) {
        LOG_PRINTLN("[DISC] No sensors found after re-discovery.");
    } else {
        LOG_PRINT("[DISC] Re-discovery complete: %d pods, %d logical channels\n",
            g_pod_count, g_logical_count);
    }
}

void reinit_sensors() {
    for (uint8_t ch = 0; ch < 3; ch++) {
        float ratio = 0.0f;
        if (settings_load_volt_ratio(ch, &ratio) && ratio > 0.0f) {
            volt_ratios[ch] = ratio;
        } else {
            float r_h = 0.0f, r_l = 0.0f;
            if (settings_load_resistors(ch, &r_h, &r_l) && r_h > 0.0f && r_l > 0.0f) {
                volt_ratios[ch] = (r_h + r_l) / r_l;
            }
        }
    }

#if ENABLE_INA3221
    for (uint8_t ch = 0; ch < 3; ch++) {
        float shunt = 0.0f;
        if (settings_load_shunt(ch, &shunt) && shunt > 0.0f) {
            ina3221.setShuntResistance(ch, shunt);
        }
    }
#endif
}

// ── Read with burst sampling ───────────────────────────────────────────────────

SensorSnapshot read_sensors() {
    SensorSnapshot snap = {0};
    snap.timestamp_ms = millis();

    for (uint8_t i = 0; i < g_pod_count; i++) {
        g_pods[i].read_fn(&g_pods[i].state);
        snap.pods[i] = g_pods[i].state;
    }
    snap.num_pods = g_pod_count;
    snap.total_logical_channels = g_logical_count;

    // Baseline calibration: accumulate stddev for first BASELINE_TICKS
#if ENABLE_BASELINE_CALIBRATION
    if (baseline_count < BASELINE_TICKS) {
        baseline_count++;
        for (int i = 0; i < 3 && i < (int)g_logical_count; i++) {
            baseline_stddev[i] += g_meta[i].stddev / (float)BASELINE_TICKS;
        }
        if (baseline_count >= BASELINE_TICKS) {
            LOG_PRINT("[CALIB] baseline complete: i0_stddev=%.4f i1_stddev=%.4f i2_stddev=%.4f\n",
                baseline_stddev[0], baseline_stddev[1], baseline_stddev[2]);
        }
    }
#endif

    return snap;
}

// ── Logical channel accessors ──────────────────────────────────────────────────

const PhysicalChannel* sensor_get_logical_channel(uint8_t logical_ch) {
    if (logical_ch >= g_logical_count) return nullptr;
    uint8_t logical = 0;
    for (uint8_t pod_idx = 0; pod_idx < g_pod_count; pod_idx++) {
        uint8_t n = g_pods[pod_idx].state.num_channels;
        if (logical_ch < logical + n) {
            return &g_pods[pod_idx].state.channels[logical_ch - logical];
        }
        logical += n;
    }
    return nullptr;
}

uint8_t sensor_get_logical_channel_count() {
    return g_logical_count;
}

SampleMeta sensor_get_meta(uint8_t logical_ch) {
    const PhysicalChannel* pc = sensor_get_logical_channel(logical_ch);
    if (!pc) return {0.0f, false};
    return pc->meta;
}

float get_channel_voltage(uint8_t ch) {
    const PhysicalChannel* pc = sensor_get_logical_channel(ch);
    return pc ? pc->voltage : 0.0f;
}

float get_channel_current(uint8_t ch) {
    const PhysicalChannel* pc = sensor_get_logical_channel(ch);
    return pc ? pc->current : 0.0f;
}

float get_channel_power(uint8_t ch) {
    const PhysicalChannel* pc = sensor_get_logical_channel(ch);
    return pc ? pc->power : 0.0f;
}

static const PhysicalChannel* logical_channel_from_snapshot(const SensorSnapshot* snap, uint8_t logical_ch) {
    if (!snap || logical_ch >= snap->total_logical_channels) return nullptr;
    uint8_t logical = 0;
    for (uint8_t i = 0; i < snap->num_pods; i++) {
        uint8_t n = snap->pods[i].num_channels;
        if (logical_ch < logical + n) {
            return &snap->pods[i].channels[logical_ch - logical];
        }
        logical += n;
    }
    return nullptr;
}

const PhysicalChannel* sensor_get_logical_channel(const SensorSnapshot& snap, uint8_t logical_ch) {
    return logical_channel_from_snapshot(&snap, logical_ch);
}

float get_channel_voltage(const SensorSnapshot& snap, uint8_t ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&snap, ch);
    return pc ? pc->voltage : 0.0f;
}

float get_channel_current(const SensorSnapshot& snap, uint8_t ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&snap, ch);
    return pc ? pc->current : 0.0f;
}

float get_channel_power(const SensorSnapshot& snap, uint8_t ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&snap, ch);
    return pc ? pc->power : 0.0f;
}

// ── Calibration / baseline API ─────────────────────────────────────────────────

void sensor_calibrate_baseline() {
#if ENABLE_BASELINE_CALIBRATION
    baseline_count = 0;
    for (int i = 0; i < MAX_LOGICAL_CHANNELS; i++) baseline_stddev[i] = 0;
    LOG_PRINTLN("Baseline recalibration started");
#else
    LOG_PRINTLN("Baseline calibration disabled at compile time");
#endif
}

void sensor_get_baseline_progress(float* stddev_out, uint8_t* tick_count_out) {
    *tick_count_out = baseline_count;
    if (stddev_out) {
        for (int i = 0; i < MAX_LOGICAL_CHANNELS; i++) stddev_out[i] = baseline_stddev[i];
    }
}

bool sensor_is_calibrating() {
#if ENABLE_BASELINE_CALIBRATION
    return baseline_count > 0 && baseline_count < BASELINE_TICKS;
#else
    return false;
#endif
}

float ina3221_getShuntVoltage(uint8_t ch) {
#if ENABLE_INA3221
    return ina3221.getShuntVoltage(ch);
#else
    (void)ch;
    return 0.0f;
#endif
}

float ina226_getShuntVoltage() {
#if ENABLE_INA226
    for (uint8_t i = 0; i < INA226_COUNT && i < MAX_INA226; i++) {
        if (ina226_devices[i]) return ina226_devices[i]->getShuntVoltage();
    }
#endif
    return 0.0f;
}

float ina3221_getVoltModuleBusVoltage(uint8_t ch) {
#if ENABLE_INA3221_VOLT
    return ina3221_volt.getBusVoltage(ch);
#else
    (void)ch;
    return 0.0f;
#endif
}

void sensor_set_calibration(uint8_t ch, uint8_t type, float value) {
    if (ch >= MAX_LOGICAL_CHANNELS) return;
    switch (type) {
        case 0: cal.volt_offset_mv[ch] = value; break;
        case 1: cal.volt_gain[ch] = value; break;
        case 2: cal.curr_offset_ma[ch] = value; break;
        case 3: cal.curr_gain[ch] = value; break;
        default: return;
    }
    settings_save_channel_calibration(&cal);
}

void sensor_get_calibration(uint8_t ch, float* volt_offset_mv, float* volt_gain, float* curr_offset_mv, float* curr_gain) {
    if (ch >= MAX_LOGICAL_CHANNELS) ch = 0; // guard, though callers should validate
    *volt_offset_mv = cal.volt_offset_mv[ch];
    *volt_gain = cal.volt_gain[ch];
    *curr_offset_mv = cal.curr_offset_ma[ch];
    *curr_gain = cal.curr_gain[ch];
}

void sensor_reset_calibration(uint8_t ch) {
    if (ch >= MAX_LOGICAL_CHANNELS) return;
    cal.volt_offset_mv[ch] = 0.0f;
    cal.volt_gain[ch] = 1.0f;
    cal.curr_offset_ma[ch] = 0.0f;
    cal.curr_gain[ch] = 1.0f;
    cal.invert_curr[ch] = false;
    settings_save_channel_calibration(&cal);
}

void sensor_set_invert_curr(uint8_t ch, bool invert) {
    if (ch >= MAX_LOGICAL_CHANNELS) return;
    cal.invert_curr[ch] = invert;
    settings_save_channel_calibration(&cal);
}

void sensor_reset_invert_curr(uint8_t ch) {
    if (ch >= MAX_LOGICAL_CHANNELS) return;
    cal.invert_curr[ch] = false;
    settings_save_channel_calibration(&cal);
}
