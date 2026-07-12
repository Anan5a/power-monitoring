// internal/fakes/builders.go — Fluent test-data builders.
// Usage: dev := aDevice("AABBCCDDEEFF").ownedBy(userID).build()

package fakes

import (
	"time"

	"github.com/yourorg/iot-platform/internal"
)

// ── Device builder ──────────────────────────────────────────────────

type DeviceBuilder struct {
	d internal.Device
}

func ADevice(key string) *DeviceBuilder {
	return &DeviceBuilder{d: internal.Device{
		DeviceKey:  key,
		DeviceName: "Test Device",
		DeviceType: "power_monitor_v2",
		IsActive:   true,
		CreatedAt:  time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC),
	}}
}

func (b *DeviceBuilder) ownedBy(userID string) *DeviceBuilder {
	b.d.OwnerID = &userID
	return b
}

func (b *DeviceBuilder) withType(t string) *DeviceBuilder {
	b.d.DeviceType = t
	return b
}

func (b *DeviceBuilder) build() *internal.Device {
	return &b.d
}

// ── TelemetryRow builder ────────────────────────────────────────────

type TelemetryRowBuilder struct {
	r internal.TelemetryRow
}

func aTelemetryRow(deviceID string) *TelemetryRowBuilder {
	return &TelemetryRowBuilder{r: internal.TelemetryRow{
		DeviceID:   deviceID,
		DeviceType: "power_monitor_v2",
		Timestamp:  time.Date(2026, 7, 12, 10, 0, 0, 0, time.UTC),
		Fields:     map[string]float64{},
	}}
}

func (b *TelemetryRowBuilder) withField(key string, val float64) *TelemetryRowBuilder {
	b.r.Fields[key] = val
	return b
}

func (b *TelemetryRowBuilder) build() internal.TelemetryRow { return b.r }

// ── MQTT message helper ─────────────────────────────────────────────

type FakeMQTTMessage struct {
	topic   string
	payload []byte
}

func (m FakeMQTTMessage) Topic() string  { return m.topic }
func (m FakeMQTTMessage) Payload() []byte { return m.payload }
func (m FakeMQTTMessage) Ack()            {}

func AMQTTMessage(topic string, payload []byte) FakeMQTTMessage {
	return FakeMQTTMessage{topic: topic, payload: payload}
}
