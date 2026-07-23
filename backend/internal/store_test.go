// internal/store_test.go — Tests for the batch writer.

package internal_test

import (
	"context"
	"testing"
	"time"

	"github.com/Anan5a/iot-platform/internal"
	"github.com/Anan5a/iot-platform/internal/fakes"
)

func TestBatchWriter_BufferAndFlush(t *testing.T) {
	store := fakes.NewMemStore()
	bw := internal.NewBatchWriter(store, nil, nil)

	bw.Write(context.Background(), internal.TelemetryRow{
		DeviceID:   "AABBCCDDEEFF",
		DeviceType: "power_monitor_v2",
		Timestamp:  time.Now(),
		Fields:     map[string]float64{"ch0_P": 19.8},
	})
	bw.Write(context.Background(), internal.TelemetryRow{
		DeviceID:   "AABBCCDDEEFF",
		DeviceType: "power_monitor_v2",
		Timestamp:  time.Now(),
		Fields:     map[string]float64{"ch0_P": 20.1},
	})

	if store.Count() != 0 {
		t.Fatalf("Count before flush = %d, want 0", store.Count())
	}

	bw.Flush(context.Background())

	if store.Count() != 2 {
		t.Errorf("Count after flush = %d, want 2", store.Count())
	}
}
