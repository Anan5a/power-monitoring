#include "sensor_manager.h"
#include "settings_manager.h"
#include "config.h"
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
static INA226 ina226;
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
    .volt_offset_mv = {CAL_VOLT_OFFSET_MV_CH0, CAL_VOLT_OFFSET_MV_CH1, CAL_VOLT_OFFSET_MV_CH2},
    .volt_gain = {CAL_VOLT_GAIN_CH0, CAL_VOLT_GAIN_CH1, CAL_VOLT_GAIN_CH2},
    .curr_offset_ma = {CAL_CURR_OFFSET_MA_CH0, CAL_CURR_OFFSET_MA_CH1, CAL_CURR_OFFSET_MA_CH2},
    .curr_gain = {CAL_CURR_GAIN_CH0, CAL_CURR_GAIN_CH1, CAL_CURR_GAIN_CH2},
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

    float cal_ma = (med - cal.curr_offset_ma[hw_ch]) * cal.curr_gain[hw_ch];
    if (fabsf(cal_ma) < 5.0f) cal_ma = 0.0f; // dead-zone
    float curr_a = cal_ma / 1000.0f;
    if (cal.invert_curr[hw_ch]) curr_a = -curr_a;
    ch->current = curr_a;
#else
    ch->current = 0.0f;
    g_meta[hw_ch] = {0.0f, false};
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
    ch->voltage = ina226.getBusVoltage() + INA226_V_OFFSET;
    ch->current = ina226.getCurrent() * INA226_I_GAIN;
    ch->power   = ina226.getPower();
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

#if ENABLE_INA3221
    if (!ina3221.begin(INA3221_ADDR, &Wire)) {
        Serial.println("INA3221 current (0x40) init failed");
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
        Serial.println("INA3221 voltage (0x42) init failed");
    }
#endif

#if ENABLE_INA226
    if (!ina226.begin()) {
        Serial.println("INA226 disabled");
    }
#else
    Serial.println("INA226 disabled");
#endif

#if ENABLE_ADS1115
    if (!ads1115.begin(ADS1115_ADDR, &Wire)) {
        Serial.println("ADS1115 init failed");
    } else {
        ads1115.setGain(GAIN_ONE);
    }
#else
    Serial.println("ADS1115 disabled");
#endif

    // Register pods: INA3221 channels first so pod id == hardware channel.
#if ENABLE_INA3221 || ENABLE_INA3221_VOLT
    for (uint8_t ch = 0; ch < 3; ch++) {
        char name[16];
        snprintf(name, sizeof(name), "CH%d", ch);
        register_pod(POD_INA226, name, 1, pod_ina3221_read);
    }
#endif

#if ENABLE_INA226
    register_pod(POD_INA226, "INA226", 1, pod_ina226_read);
#endif
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
            Serial.printf("[CALIB] baseline complete: i0_stddev=%.4f i1_stddev=%.4f i2_stddev=%.4f\n",
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
    Serial.println("Baseline recalibration started");
#else
    Serial.println("Baseline calibration disabled at compile time");
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
    return ina226.getShuntVoltage();
#else
    return 0.0f;
#endif
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
    if (ch >= 3) return;
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
    if (ch >= 3) ch = 0; // guard, though callers should validate
    *volt_offset_mv = cal.volt_offset_mv[ch];
    *volt_gain = cal.volt_gain[ch];
    *curr_offset_mv = cal.curr_offset_ma[ch];
    *curr_gain = cal.curr_gain[ch];
}

void sensor_reset_calibration(uint8_t ch) {
    if (ch >= 3) return;
    cal.volt_offset_mv[ch] = 0.0f;
    cal.volt_gain[ch] = 1.0f;
    cal.curr_offset_ma[ch] = 0.0f;
    cal.curr_gain[ch] = 1.0f;
    cal.invert_curr[ch] = false;
    settings_save_channel_calibration(&cal);
}

void sensor_set_invert_curr(uint8_t ch, bool invert) {
    if (ch >= 3) return;
    cal.invert_curr[ch] = invert;
    settings_save_channel_calibration(&cal);
}

void sensor_reset_invert_curr(uint8_t ch) {
    if (ch >= 3) return;
    cal.invert_curr[ch] = false;
    settings_save_channel_calibration(&cal);
}
