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

	"github.com/jackc/pgx/v5/pgxpool"
)

// ── Interfaces ──────────────────────────────────────────────

// Clock is a small abstraction over time.Now so pipeline logic can be
// tested deterministically with a fake clock.
type Clock interface {
	Now() time.Time
}

// RealClock returns the wall clock; the zero-value struct is stateless.
type RealClock struct{}

// Now implements Clock using time.Now.
func (RealClock) Now() time.Time { return time.Now() }

// MQTTPublisher is the minimal publish surface the pipeline needs from an
// MQTT client. Tests supply a fake; production uses paho.
type MQTTPublisher interface {
	Publish(topic string, qos byte, retained bool, payload []byte) error
}

// DeviceResolver looks up a device by its device_key (the MQTT topic's
// last segment). The narrow surface lets tests substitute a fake.
type DeviceResolver interface {
	Resolve(ctx context.Context, deviceKey string) (*Device, error)
}

// ── MQTT Message ────────────────────────────────────────────

// MQTTMessage is the minimal subset of paho's message interface used by
// the pipeline. Isolating it avoids importing the MQTT client here.
type MQTTMessage interface {
	Topic() string
	Payload() []byte
	Ack()
}

// ── Ingest Pipeline ─────────────────────────────────────────

// Pipeline is the ingest worker's hot path: one message in, one row
// written and republished. It is stateless and safe to call concurrently
// from multiple goroutines provided its dependencies are.
type Pipeline struct {
	resolver DeviceResolver
	enricher *Enricher
	store    TelemetryStore
	mqtt     MQTTPublisher
	clock    Clock
}

// NewPipeline wires the pipeline's dependencies. Pass a non-nil clock for
// deterministic tests; nil is acceptable for production but will panic on use.
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
	// Raw payload is a flat JSON object; we decode only the well-known
	// envelope fields and keep the per-channel `data` map verbatim so
	// the store can persist any channel the firmware sends.
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

	deviceKey := ExtractDeviceKey(msg.Topic())
	device, err := p.resolver.Resolve(ctx, deviceKey)
	if err != nil {
		return fmt.Errorf("resolve device %s: %w", deviceKey, err)
	}

	// Enrich
	enriched := p.enricher.Enrich(raw.Data, nil) // groups loaded from device config in Phase 2

	// Store
	// Reconstruct the timestamp from the device-supplied seconds + millis.
	// time.Unix takes (sec, nsec), so millis are scaled up by 1e6.
	ts := time.Unix(raw.Ts, int64(raw.TsMS)*1_000_000)
	if err := p.store.Write(ctx, TelemetryRow{
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
	}); err != nil {
		return fmt.Errorf("store write: %w", err)
	}

	// Republish to live/{device_key}
	// Marshal errors are intentionally ignored: the struct is plain JSON
	// and a failure here is non-fatal — the row is already persisted.
	livePayload, _ := json.Marshal(EnrichedTelemetry{
		DeviceKey:     deviceKey,
		DeviceType:    device.DeviceType,
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
	// QoS 0, not retained: live frames are ephemeral and only the newest
	// matters to subscribers (websocket, alert engine).
	if err := p.mqtt.Publish("live/"+deviceKey, 0, false, livePayload); err != nil {
		slog.Warn("republish failed", "device", deviceKey, "error", err)
	}

	return nil
}

// ── Device Resolver (PostgreSQL) ─────────────────────────────────────

// PGDeviceResolver resolves devices from PostgreSQL. It caches nothing:
// the ingest hot path hits Postgres per message, relying on its
// connection pool and prepared-statement cache for throughput.
type PGDeviceResolver struct {
	pg *pgxpool.Pool
}

// NewDeviceResolver returns a PGDeviceResolver backed by the given pool.
func NewDeviceResolver(pg *pgxpool.Pool) *PGDeviceResolver {
	return &PGDeviceResolver{pg: pg}
}

// Resolve implements DeviceResolver. owner_id is cast to text on the
// server side so pgx can scan it into a string without a UUID import.
func (r *PGDeviceResolver) Resolve(ctx context.Context, deviceKey string) (*Device, error) {
	var d Device
	err := r.pg.QueryRow(ctx,
		`SELECT id, device_key, device_name, device_type, owner_id::text, is_active,
		        coalesce(firmware_ver, ''), last_seen_at, created_at
		 FROM devices WHERE device_key = $1`,
		deviceKey).Scan(
		&d.ID, &d.DeviceKey, &d.DeviceName, &d.DeviceType, &d.OwnerID,
		&d.IsActive, &d.FirmwareVer, &d.LastSeenAt, &d.CreatedAt)
	if err != nil {
		return nil, fmt.Errorf("resolve device %s: %w", deviceKey, err)
	}
	return &d, nil
}

// ExtractDeviceKey pulls the device key out of a telemetry topic of the
// form "telemetry/{device_type}/{device_key}". If the topic does not have
// the expected three segments the whole topic is returned so the caller
// still gets a stable (if unhelpful) lookup key.
func ExtractDeviceKey(topic string) string {
	// topic = "telemetry/{device_type}/{device_key}"
	parts := splitTopic(topic)
	if len(parts) >= 3 {
		return parts[2]
	}
	return topic
}

// splitTopic splits an MQTT topic on '/'. It avoids strings.Split (and its
// substrings slice) and is allocation-light when the topic has few segments.
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
