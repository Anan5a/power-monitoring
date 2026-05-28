#include "sensor_manager.h"
#include "settings_manager.h"
#include "config.h"
#include <Wire.h>
#include <algorithm>

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

// Burst sample metadata — accessible by connectivity_manager for calibration status
SampleMeta g_meta[8] = {{0,false},{0,false},{0,false},{0,false},{0,false},{0,false},{0,false},{0,false}};
float baseline_stddev[8] = {0};
uint8_t baseline_count = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

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

// ── Init ─────────────────────────────────────────────────────────────────────

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
}

// ── Read with burst sampling ──────────────────────────────────────────────────

SensorData read_sensors() {
    SensorData d = {0};
    float samples[BURST_N];

    // ── INA3221 current (0x40) ─────────────────────────────────
#if ENABLE_INA3221
    for (uint8_t ch = 0; ch < 3; ch++) {
        for (int i = 0; i < BURST_N; i++) {
            samples[i] = ina3221.getCurrentAmps(ch) * 1000.0f; // mA
        }
        float med = median_of(samples, BURST_N);
        float sd = stddev_of(samples, BURST_N, med);
        float max_dev = max_deviation(samples, BURST_N, med);
        bool spike = baseline_count >= BASELINE_TICKS &&
                     sd > SPIKE_STDDEV_MULT * baseline_stddev[ch] &&
                     max_dev > SPIKE_DEVIATION_MA;
        g_meta[ch] = {sd, spike};

        float cal_ma = (med - cal.curr_offset_ma[ch]) * cal.curr_gain[ch];
        if (fabsf(cal_ma) < 5.0f) cal_ma = 0.0f; // dead-zone
        d.ina3221_current[ch] = cal_ma / 1000.0f;
    }
#else
    for (uint8_t ch = 0; ch < 3; ch++) {
        g_meta[ch] = {0, false};
        d.ina3221_current[ch] = 0;
    }
#endif

    // ── INA3221 voltage (0x42) ─────────────────────────────────
#if ENABLE_INA3221_VOLT
    for (uint8_t ch = 0; ch < 3; ch++) {
        for (int i = 0; i < BURST_N; i++) {
            samples[i] = ina3221_volt.getBusVoltage(ch) * 1000.0f; // mV
        }
        float med = median_of(samples, BURST_N);
        float sd = stddev_of(samples, BURST_N, med);
        float max_dev = max_deviation(samples, BURST_N, med);
        bool spike = baseline_count >= BASELINE_TICKS &&
                     sd > SPIKE_STDDEV_MULT * baseline_stddev[ch + 3] &&
                     max_dev > SPIKE_DEVIATION_MV;
        g_meta[ch + 3] = {sd, spike};

        float cal_mv = (med + cal.volt_offset_mv[ch]) * cal.volt_gain[ch];
        d.ina3221_busV[ch] = cal_mv / 1000.0f * volt_ratios[ch];
        d.ads1115_volts[ch] = d.ina3221_busV[ch];
    }
#else
    for (uint8_t ch = 0; ch < 3; ch++) {
        g_meta[ch + 3] = {0, false};
        d.ads1115_volts[ch] = 0;
    }
#endif

    // ── INA226 (single read, already filtered by HW) ────────────
#if ENABLE_INA226
    d.ina226_busV    = ina226.getBusVoltage() + INA226_V_OFFSET;
    d.ina226_current = ina226.getCurrent() * INA226_I_GAIN;
    d.ina226_power   = ina226.getPower();
    g_meta[6] = {0, false};
#endif

    // ── ADS1115 ───────────────────────────────────────────────
#if ENABLE_ADS1115
    for (uint8_t ch = 0; ch < 4; ch++) {
        for (int i = 0; i < BURST_N; i++) {
            int16_t raw = ads1115.readADC_SingleEnded(ch);
            samples[i] = ads1115.computeVolts(raw);
        }
        float med = median_of(samples, BURST_N);
        d.ads1115_volts[ch] = med;
    }
#endif

    // Baseline calibration: accumulate stddev for first BASELINE_TICKS
    if (baseline_count < BASELINE_TICKS) {
        baseline_count++;
        for (int i = 0; i < 6; i++) { // ch 0-5 (INA3221 current + voltage)
            baseline_stddev[i] += g_meta[i].stddev / (float)BASELINE_TICKS;
        }
        if (baseline_count >= BASELINE_TICKS) {
            Serial.printf("[CALIB] baseline complete: i0_stddev=%.4f i1_stddev=%.4f i2_stddev=%.4f\n",
                baseline_stddev[0], baseline_stddev[1], baseline_stddev[2]);
        }
    }

    return d;
}

// ── Public API ────────────────────────────────────────────────────────────────

SampleMeta sensor_get_meta(uint8_t ch) {
    if (ch >= 8) return {0, false};
    return g_meta[ch];
}

// burst sample metadata — accessible by connectivity_manager for calibration status reporting
extern SampleMeta g_meta[8];
extern float baseline_stddev[8];
extern uint8_t baseline_count;

void sensor_calibrate_baseline() {
    baseline_count = 0;
    for (int i = 0; i < 8; i++) baseline_stddev[i] = 0;
    Serial.println("Baseline recalibration started");
}

void sensor_get_baseline_progress(float* stddev_out, uint8_t* tick_count_out) {
    *tick_count_out = baseline_count;
    if (stddev_out) {
        for (int i = 0; i < 8; i++) stddev_out[i] = baseline_stddev[i];
    }
}

bool sensor_is_calibrating() {
    return baseline_count > 0 && baseline_count < BASELINE_TICKS;
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

#if !ENABLE_INA3221
float ina3221_getShuntVoltage(uint8_t) { return 0.0f; }
#endif

#if !ENABLE_INA3221_VOLT
float ina3221_getVoltModuleBusVoltage(uint8_t) { return 0.0f; }
#else
float ina3221_getVoltModuleBusVoltage(uint8_t ch) {
    return ina3221_volt.getBusVoltage(ch);
}
#endif

void sensor_set_calibration(uint8_t ch, uint8_t type, float value) {
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
    *volt_offset_mv = cal.volt_offset_mv[ch];
    *volt_gain = cal.volt_gain[ch];
    *curr_offset_mv = cal.curr_offset_ma[ch];
    *curr_gain = cal.curr_gain[ch];
}

void sensor_reset_calibration(uint8_t ch) {
    cal.volt_offset_mv[ch] = 0.0f;
    cal.volt_gain[ch] = 1.0f;
    cal.curr_offset_ma[ch] = 0.0f;
    cal.curr_gain[ch] = 1.0f;
    settings_save_channel_calibration(&cal);
}