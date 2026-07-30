#include "telemetry_pb.h"
#include <pb_encode.h>
#include <string.h>

// Namespace the nanopb-generated structs to avoid name collision with the
// C++ TelemetrySnapshot (telemetry.h). Both define a type called
// TelemetrySnapshot with different layouts.
namespace pb {
#include "telemetry.pb.h"
}

size_t encode_telemetry_pb(const TelemetrySnapshot& snap, uint8_t* out, size_t out_len) {
    if (!out || out_len < 256) return 0;

    pb::TelemetrySnapshot msg = {};  // zero-initialized

    msg.ts = snap.ts;
    msg.ts_ms = snap.ts_ms;
    msg.schema_version = snap.schema_version;
    // schema: copy C string into fixed-size byte array
    size_t slen = strlen(snap.schema);
    if (slen > sizeof(msg.schema.bytes)) slen = sizeof(msg.schema.bytes);
    memcpy(msg.schema.bytes, snap.schema, slen);
    msg.schema.size = slen;

    // device
    msg.has_device = true;
    size_t idlen = strlen(snap.device.id);
    if (idlen > sizeof(msg.device.id.bytes)) idlen = sizeof(msg.device.id.bytes);
    memcpy(msg.device.id.bytes, snap.device.id, idlen);
    msg.device.id.size = idlen;
    size_t fwlen = strlen(snap.device.fw);
    if (fwlen > sizeof(msg.device.fw.bytes)) fwlen = sizeof(msg.device.fw.bytes);
    memcpy(msg.device.fw.bytes, snap.device.fw, fwlen);
    msg.device.fw.size = fwlen;
    msg.device.uptime_ms = snap.device.uptime_ms;

    // wifi
    msg.has_wifi = true;
    msg.wifi.rssi = snap.wifi.rssi;
    size_t iplen = strlen(snap.wifi.ip);
    if (iplen > sizeof(msg.wifi.ip.bytes)) iplen = sizeof(msg.wifi.ip.bytes);
    memcpy(msg.wifi.ip.bytes, snap.wifi.ip, iplen);
    msg.wifi.ip.size = iplen;

    // channels
    msg.channel_count = snap.channel_count;
    msg.channels_count = (snap.channel_count <= 16) ? snap.channel_count : 16;
    for (uint8_t i = 0; i < msg.channels_count; i++) {
        const TelemetryChannel& src = snap.channels[i];
        pb::TelemetryChannel& dst = msg.channels[i];
        dst.ch = src.ch;
        dst.V = src.V;
        dst.I = src.I;
        dst.P = src.P;
        dst.energy_Wh = src.energy_Wh;
        dst.charge_mAh = src.charge_mAh;
    }

    // switches
    msg.switch_count = snap.switch_count;
    msg.switches_count = (snap.switch_count <= 8) ? snap.switch_count : 8;
    for (uint8_t i = 0; i < msg.switches_count; i++) {
        const TelemetrySwitch& src = snap.switches[i];
        pb::TelemetrySwitch& dst = msg.switches[i];
        dst.idx = src.idx;
        dst.type = src.type;
        dst.state = src.state;
        dst.auto_mode = src.auto_mode;
        dst.rule_tripped = src.rule_tripped;
    }

    // battery
    msg.num_batteries = snap.battery_count;
    msg.battery_count = (snap.battery_count <= 8) ? snap.battery_count : 8;
    for (uint8_t i = 0; i < msg.battery_count; i++) {
        const TelemetryBattery& src = snap.battery[i];
        pb::TelemetryBattery& dst = msg.battery[i];
        dst.ch = src.ch;
        dst.profile_id = src.profile_id;
        dst.chemistry = src.chemistry;
        dst.rated_Ah = src.rated_Ah;
        dst.soc_pct = src.soc_pct;
        dst.V = src.V;
        dst.I = src.I;
        dst.cumulative_Ah_in = src.cumulative_Ah_in;
        dst.cumulative_Ah_out = src.cumulative_Ah_out;
        dst.equivalent_full_cycles = src.equivalent_full_cycles;
        dst.soh_pct = src.soh_pct;
        dst.soh_samples = src.soh_samples;
    }

    // log meta
    msg.has_log = true;
    msg.log.entries = snap.log.entries;
    msg.log.overflow = snap.log.overflow;

    msg.heap_free = snap.heap_free;

    // Encode. The fields descriptor is declared in the global namespace by
    // the generated telemetry.pb.c; reference it via :: prefix.
    extern const pb_msgdesc_t TelemetrySnapshot_msg;
    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, &TelemetrySnapshot_msg, &msg)) {
        return 0;
    }
    return stream.bytes_written;
}
