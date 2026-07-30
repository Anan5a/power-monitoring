#ifndef TELEMETRY_PB_H
#define TELEMETRY_PB_H

#include <stddef.h>
#include <stdint.h>
#include "telemetry.h"

// Encode a TelemetrySnapshot into a protobuf byte buffer.
// Returns the number of bytes written, or 0 on overflow (same contract as
// serialize_telemetry in connectivity_manager.cpp).
// The buffer must be at least TELEMETRY_BUF_BYTES (4096) bytes.
size_t encode_telemetry_pb(const TelemetrySnapshot& snap, uint8_t* out, size_t out_len);

#endif // TELEMETRY_PB_H
