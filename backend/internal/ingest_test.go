// internal/ingest_test.go — Tests for the MQTT ingest pipeline.

package internal

import (
	"context"
	"testing"

	"github.com/yourorg/iot-platform/internal/fakes"
)

func TestPipeline_Process_StoresAndRepublishes(t *testing.T) {
	clock := fakes.ParseClock("2026-07-12T10:00:00Z")
	pub := &fakes.FakePublisher{}
	store := fakes.NewMemStore()
	enricher := NewEnricher()

	pipe := &Pipeline{
		resolver: &fakes.StubResolver{Device: fakes.ADevice("AABBCCDDEEFF").build()},
		enricher: enricher,
		store:    store,
		mqtt:     pub,
		clock:    clock,
	}

	payload := []byte(`{
		"ts": 1720000000, "ts_ms": 0, "schema": "telemetry_v1", "fw": "2.0.0",
		"uptime_ms": 3600000, "rssi": -55, "heap_free": 150000,
		"data": {"ch0_P": 19.8, "ch1_P": -6.4}
	}`)

	msg := fakes.AMQTTMessage("telemetry/power_monitor_v2/AABBCCDDEEFF", payload)
	err := pipe.Process(context.Background(), msg)
	if err != nil {
		t.Fatalf("Process() error = %v", err)
	}

	if store.Count() != 1 {
		t.Errorf("store count = %d, want 1", store.Count())
	}

	last := pub.LastMessage()
	if last == nil {
		t.Fatal("no message published")
	}
	if last.Topic != "live/AABBCCDDEEFF" {
		t.Errorf("topic = %q, want live/AABBCCDDEEFF", last.Topic)
	}
}

func TestExtractDeviceKey(t *testing.T) {
	tests := []struct {
		topic string
		want  string
	}{
		{"telemetry/power_monitor_v2/AABBCCDDEEFF", "AABBCCDDEEFF"},
		{"telemetry/temp_sensor/1234", "1234"},
		{"status/AABBCCDDEEFF/online", "online"},
	}
	for _, tt := range tests {
		got := extractDeviceKey(tt.topic)
		if got != tt.want {
			t.Errorf("extractDeviceKey(%q) = %q, want %q", tt.topic, got, tt.want)
		}
	}
}
