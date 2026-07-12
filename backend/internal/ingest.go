// internal/ingest.go — MQTT consumer pipeline. Runs in the ingest worker.
// Pure data plumbing: parse → validate → enrich → store → republish.
// No business logic (alerts, email) — that lives in the API.

package internal

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"time"
)

// ── Interfaces ──────────────────────────────────────────────────────

type Clock interface {
	Now() time.Time
}

type RealClock struct{}

func (RealClock) Now() time.Time { return time.Now() }

type MQTTPublisher interface {
	Publish(topic string, qos byte, retained bool, payload []byte) error
}

type DeviceResolver interface {
	Resolve(ctx context.Context, deviceKey string) (*Device, error)
}

// ── MQTT Message ────────────────────────────────────────────────────

type MQTTMessage interface {
	Topic() string
	Payload() []byte
	Ack()
}

// ── Ingest Pipeline ─────────────────────────────────────────────────

type Pipeline struct {
	resolver DeviceResolver
	enricher *Enricher
	store    TelemetryStore
	mqtt     MQTTPublisher
	clock    Clock
}

func NewPipeline(resolver DeviceResolver, enricher *Enricher, store TelemetryStore, mqtt MQTTPublisher, clock Clock) *Pipeline {
	return &Pipeline{
		resolver: resolver,
		enricher: enricher,
		store:    store,
		mqtt:     mqtt,
		clock:    clock,
	}
}

// Process handles one MQTT message. It is the hot path — keep it fast.
func (p *Pipeline) Process(ctx context.Context, msg MQTTMessage) error {
	var raw struct {
		Ts       int64              `json:"ts"`
		TsMS     int                `json:"ts_ms"`
		Schema   string             `json:"schema"`
		FW       string             `json:"fw"`
		UptimeMS uint32             `json:"uptime_ms"`
		RSSI     int8               `json:"rssi"`
		HeapFree uint32             `json:"heap_free"`
		Data     map[string]float64 `json:"data"`
	}
	if err := json.Unmarshal(msg.Payload(), &raw); err != nil {
		return fmt.Errorf("parse payload: %w", err)
	}

	deviceKey := extractDeviceKey(msg.Topic())
	device, err := p.resolver.Resolve(ctx, deviceKey)
	if err != nil {
		return fmt.Errorf("resolve device %s: %w", deviceKey, err)
	}

	// Enrich
	enriched := p.enricher.Enrich(raw.Data, nil) // groups loaded from device config in Phase 2

	// Store
	ts := time.Unix(raw.Ts, int64(raw.TsMS)*1_000_000)
	p.store.Write(ctx, TelemetryRow{
		DeviceID:      deviceKey,
		DeviceType:    device.DeviceType,
		Timestamp:     ts,
		RSSI:          raw.RSSI,
		UptimeMS:      raw.UptimeMS,
		PVPower:       enriched.PVPower,
		BatteryPower:  enriched.BatteryPower,
		InverterPower: enriched.InverterPower,
		DCLoadPower:   enriched.DCLoadPower,
		SystemStatus:  enriched.SystemStatus,
		MinSOCPct:     enriched.MinSOCPct,
		MaxSOCPct:     enriched.MaxSOCPct,
		TotalEnergyWh: enriched.TotalEnergyWh,
		Fields:        raw.Data,
	})

	// Republish to live/{device_key}
	livePayload, _ := json.Marshal(EnrichedTelemetry{
		DeviceKey:     deviceKey,
		Timestamp:     raw.Ts,
		TimestampMS:   raw.TsMS,
		Schema:        raw.Schema,
		FW:            raw.FW,
		UptimeMS:      raw.UptimeMS,
		RSSI:          raw.RSSI,
		HeapFree:      raw.HeapFree,
		PVPower:       enriched.PVPower,
		BatteryPower:  enriched.BatteryPower,
		InverterPower: enriched.InverterPower,
		DCLoadPower:   enriched.DCLoadPower,
		SystemStatus:  enriched.SystemStatus,
		MinSOCPct:     enriched.MinSOCPct,
		MaxSOCPct:     enriched.MaxSOCPct,
		TotalEnergyWh: enriched.TotalEnergyWh,
		Fields:        raw.Data,
	})
	if err := p.mqtt.Publish("live/"+deviceKey, 0, false, livePayload); err != nil {
		slog.Warn("republish failed", "device", deviceKey, "error", err)
	}

	return nil
}

func extractDeviceKey(topic string) string {
	// topic = "telemetry/{device_type}/{device_key}"
	parts := splitTopic(topic)
	if len(parts) >= 3 {
		return parts[2]
	}
	return topic
}

func splitTopic(topic string) []string {
	var parts []string
	start := 0
	for i := 0; i < len(topic); i++ {
		if topic[i] == '/' {
			parts = append(parts, topic[start:i])
			start = i + 1
		}
	}
	parts = append(parts, topic[start:])
	return parts
}
