#include "bl0939_pod.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

// Reference values derived from BL0939 datasheet / ESPHome driver.
#define BL0939_V_REF    17166.6f
#define BL0939_I_REF    266012.0f
#define BL0939_P_REF    713.19f

static const uint8_t bl0939_addresses[MAX_BL0939] = BL0939_ADDRESSES;

// Map from pod id (assigned by sensor_manager at registration) to BL0939 slot
// (index into BL0939_ADDRESSES). -1 means "not a BL0939 pod".
static int8_t bl0939_slot_for_pod[MAX_PODS];
void bl0939_set_pod_slot(uint8_t pod_id, uint8_t slot) {
    if (pod_id < MAX_PODS && slot < BL0939_COUNT) {
        bl0939_slot_for_pod[pod_id] = (int8_t)slot;
    }
}

// Per-slot cache of the most recent parsed frame, so the shared UART can be
// drained once and each pod reads its own slot. Fixes the old behavior where
// every parsed frame was written into whichever PodState was passed.
struct BL0939Cache { float v, ia, ib, pa, pb; bool valid; };
static BL0939Cache bl0939_cache[MAX_BL0939];

static int8_t bl0939_slot_for_address(uint8_t addr) {
    for (uint8_t i = 0; i < BL0939_COUNT && i < MAX_BL0939; i++) {
        if (bl0939_addresses[i] == addr) return (int8_t)i;
    }
    return -1;
}

static uint32_t bl0939_u24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static int32_t bl0939_s24(const uint8_t* p) {
    uint32_t u = bl0939_u24(p);
    if (u & 0x800000) u |= 0xFF000000;
    return (int32_t)u;
}

static bool bl0939_parse_frame(const uint8_t* buf) {
    uint8_t addr = buf[0];
    int8_t slot = bl0939_slot_for_address(addr);
    if (slot < 0) return false;

    uint8_t checksum = 0x55;
    for (uint8_t i = 0; i < BL0939_FRAME_LEN - 1; i++) checksum += buf[i];
    checksum ^= 0xFF;
    if (checksum != buf[BL0939_FRAME_LEN - 1]) return false;

    BL0939Cache c;
    c.v  = (float)bl0939_u24(buf + 1)  / BL0939_V_REF;
    c.ia = (float)bl0939_s24(buf + 4)  / BL0939_I_REF;
    c.ib = (float)bl0939_s24(buf + 7)  / BL0939_I_REF;
    c.pa = (float)bl0939_s24(buf + 10) / BL0939_P_REF;
    c.pb = (float)bl0939_s24(buf + 13) / BL0939_P_REF;
    c.valid = true;
    (void)buf[16]; (void)buf[19]; // sa/sb settling values not surfaced yet
    (void)buf[22];                // frequency byte, not used yet

    bl0939_cache[slot] = c;
    return true;
}

#if ENABLE_BL0939

// Map the configured UART number to the matching Arduino HardwareSerial object.
#if BL0939_UART_NUM == 1
    #define BL0939_SERIAL Serial1
#elif BL0939_UART_NUM == 2
    #define BL0939_SERIAL Serial2
#else
    #define BL0939_SERIAL Serial
#endif

// Drain the shared UART and route every complete frame into the matching
// slot's cache. Called from bl0939_pod_read; safe to call per-pod because a
// drained buffer simply yields no frames on the next call.
static void bl0939_drain_uart() {
    static uint8_t rx_buf[BL0939_FRAME_LEN];
    static uint8_t rx_pos = 0;

    while (BL0939_SERIAL.available()) {
        int c = BL0939_SERIAL.read();
        if (c < 0) break;
        uint8_t b = (uint8_t)c;

        if (rx_pos == 0) {
            if (bl0939_slot_for_address(b) >= 0) {
                rx_buf[rx_pos++] = b;
            }
            continue;
        }
        rx_buf[rx_pos++] = b;
        if (rx_pos >= BL0939_FRAME_LEN) {
            bl0939_parse_frame(rx_buf);
            rx_pos = 0;
        }
    }
}

void bl0939_pod_init() {
#if defined(ESP32)
    BL0939_SERIAL.begin(BL0939_BAUD, SERIAL_8N2, BL0939_RX_PIN, BL0939_TX_PIN);
#else
    BL0939_SERIAL.begin(BL0939_BAUD);
#endif
}

void bl0939_pod_read(PodState* pod) {
    if (!pod) return;
    bl0939_drain_uart();
    if (pod->id >= MAX_PODS) return;
    int8_t slot = bl0939_slot_for_pod[pod->id];
    if (slot < 0 || slot >= (int8_t)BL0939_COUNT) return;
    const BL0939Cache& c = bl0939_cache[slot];
    if (!c.valid) return;
    if (pod->num_channels >= 1) {
        pod->channels[0].voltage = c.v;
        pod->channels[0].current = c.ia;
        pod->channels[0].power   = c.pa;
    }
    if (pod->num_channels >= 2) {
        pod->channels[1].voltage = c.v;
        pod->channels[1].current = c.ib;
        pod->channels[1].power   = c.pb;
    }
}

#else // !ENABLE_BL0939

// Production builds that disable the BL0939 driver still want the public
// entry points to be no-ops, but the parser / address-lookup helpers
// above are kept so the test target (which compiles with
// BL0939_EXPOSE_FOR_TESTING=1) can link them via the *_for_test hooks.
void bl0939_pod_init() {}
void bl0939_pod_read(PodState*) {}

#endif // ENABLE_BL0939

// Test-only hook: when BL0939_EXPOSE_FOR_TESTING is defined the static
// frame parser is exposed so host-side tests can verify the CRC
// algorithm against a known frame without spinning up a UART. Used by
// sim/test_bl0939_crc. Compiled regardless of ENABLE_BL0939 because the
// helpers above are also always compiled; the production linker drops
// the symbols if the test target isn't built. Not part of the
// production ABI; production firmware never sees these symbols.
#ifdef BL0939_EXPOSE_FOR_TESTING
bool bl0939_parse_frame_for_test(const uint8_t* buf, PodState* pod) {
    // Test hook: parse into a temp cache slot then copy into the passed pod
    // so the test doesn't need a real UART. Use slot 0's cache as scratch.
    bl0939_cache[0] = BL0939Cache{};
    if (!bl0939_parse_frame(buf)) return false;
    if (pod) {
        if (pod->num_channels >= 1) {
            pod->channels[0].voltage = bl0939_cache[0].v;
            pod->channels[0].current = bl0939_cache[0].ia;
            pod->channels[0].power   = bl0939_cache[0].pa;
        }
        if (pod->num_channels >= 2) {
            pod->channels[1].voltage = bl0939_cache[0].v;
            pod->channels[1].current = bl0939_cache[0].ib;
            pod->channels[1].power   = bl0939_cache[0].pb;
        }
    }
    return true;
}
int8_t bl0939_slot_for_address_for_test(uint8_t addr) {
    return bl0939_slot_for_address(addr);
}
#endif
