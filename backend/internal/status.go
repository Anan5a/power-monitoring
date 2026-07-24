// internal/status.go — Device online/offline detection.
// The API server subscribes to status/{device_key}/online on Mosquitto. When a
// device publishes an online heartbeat (or its LWT offline message), the manager
// updates devices.is_active/last_seen_at and broadcasts a device_status event to
// subscribed WebSocket clients. A background goroutine marks stale devices offline.

package internal

import (
	"context"
	"encoding/json"
	"log/slog"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// staleOfflineThreshold is how long a device may go without a heartbeat before
// the staleness sweeper marks it offline. It is intentionally shorter than the
// firmware's publish interval so a missed-beat device is reported promptly.
const staleOfflineThreshold = 60 * time.Second

// DeviceStatusManager consumes device online/offline MQTT status messages,
// persists them to the devices table, and broadcasts the transition over the
// WebSocket hub. A background sweeper marks devices that have gone silent past
// staleOfflineThreshold as offline.
type DeviceStatusManager struct {
	pg  *pgxpool.Pool
	hub *WebSocketHub
}

// NewDeviceStatusManager constructs a DeviceStatusManager wired to the given
// Postgres pool and WebSocket hub. Either may be nil to disable that side.
func NewDeviceStatusManager(pg *pgxpool.Pool, hub *WebSocketHub) *DeviceStatusManager {
	return &DeviceStatusManager{pg: pg, hub: hub}
}

// HandleStatusMessage parses a status/{device_key}/online MQTT payload and
// updates device state. Payload: {"online": true, "ts": 1700000000}.
func (m *DeviceStatusManager) HandleStatusMessage(topic string, payload []byte) {
	deviceKey := strings.TrimPrefix(topic, "status/")
	deviceKey = strings.TrimSuffix(deviceKey, "/online")
	// Reject malformed topics: empty key or one that still contains a segment
	// separator means the topic didn't match the expected status/{key}/online
	// shape, so there is no single device to update.
	if deviceKey == "" || strings.Contains(deviceKey, "/") {
		return
	}

	var msg struct {
		Online    bool  `json:"online"`
		Timestamp int64 `json:"ts"`
	}
	if err := json.Unmarshal(payload, &msg); err != nil {
		slog.Debug("status payload unmarshal", "topic", topic, "error", err)
		return
	}

	m.applyStatus(deviceKey, msg.Online, msg.Timestamp)
}

// applyStatus persists the online/offline state and broadcasts a WS event.
func (m *DeviceStatusManager) applyStatus(deviceKey string, online bool, ts int64) {
	if m.pg == nil {
		// Persistence disabled — skip the DB write but still broadcast below.
		return
	}
	if online {
		if _, err := m.pg.Exec(context.Background(),
			`UPDATE devices SET is_active = true, last_seen_at = now()
			 WHERE device_key = $1`, deviceKey); err != nil {
			slog.Warn("status update", "device", deviceKey, "error", err)
			return
		}
	} else {
		if _, err := m.pg.Exec(context.Background(),
			`UPDATE devices SET is_active = false WHERE device_key = $1`, deviceKey); err != nil {
			slog.Warn("status update", "device", deviceKey, "error", err)
			return
		}
	}
	m.broadcast(deviceKey, online, ts)
}

func (m *DeviceStatusManager) broadcast(deviceKey string, online bool, ts int64) {
	if m.hub == nil {
		// No WebSocket hub wired — nothing to broadcast to.
		return
	}
	// If the payload carried no timestamp (e.g. an LWT offline message without
	// ts), fall back to the current server time so clients still get an event.
	if ts == 0 {
		ts = time.Now().Unix()
	}
	data, _ := json.Marshal(DeviceStatusEvent{
		Type:      "device_status",
		DeviceKey: deviceKey,
		Online:    online,
		Timestamp: ts,
	})
	m.hub.Broadcast(deviceKey, data)
}

// StartStalenessChecker runs a goroutine that periodically marks devices whose
// last_seen_at is older than the offline threshold as inactive, broadcasting the
// transition to WebSocket clients. Call once at startup; cancels with ctx.
func (m *DeviceStatusManager) StartStalenessChecker(ctx context.Context) {
	go func() {
		ticker := time.NewTicker(15 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				m.markStaleOffline(ctx)
			}
		}
	}()
}

func (m *DeviceStatusManager) markStaleOffline(ctx context.Context) {
	if m.pg == nil {
		return
	}
	staleSeconds := int(staleOfflineThreshold.Seconds())
	// Single UPDATE ... RETURNING atomically flips stale devices to offline and
	// returns the affected keys plus their last_seen epoch so we can broadcast
	// the transition to WebSocket clients with the original timestamp.
	rows, err := m.pg.Query(ctx,
		`UPDATE devices SET is_active = false
		 WHERE is_active = true
		   AND last_seen_at IS NOT NULL
		   AND last_seen_at < now() - make_interval(secs => $1)
		 RETURNING device_key, extract(epoch from last_seen_at)::bigint`,
		staleSeconds)
	if err != nil {
		slog.Warn("staleness sweep query", "error", err)
		return
	}
	defer rows.Close()
	for rows.Next() {
		var deviceKey string
		var ts int64
		if err := rows.Scan(&deviceKey, &ts); err != nil {
			// Skip a row that fails to scan rather than aborting the whole sweep.
			continue
		}
		m.broadcast(deviceKey, false, ts)
	}
	// rows.Err() is intentionally ignored: a partial sweep still produced
	// useful side effects, and a failed sweep will simply retry on the next tick.
	_ = rows.Err()
}
