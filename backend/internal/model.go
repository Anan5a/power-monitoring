// internal/model.go — Shared data types used across both binaries.
// Every exported type has a doc comment explaining its purpose.

package internal

import "time"

// ── Auth ────────────────────────────────────────────────────────────

type RegisterRequest struct {
	Email       string `json:"email"`
	Password    string `json:"password"`
	DisplayName string `json:"display_name,omitempty"`
}

type LoginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

type AuthResponse struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	User         User   `json:"user"`
}

type User struct {
	ID          string    `json:"id"`
	Email       string    `json:"email"`
	DisplayName string    `json:"display_name,omitempty"`
	Role        string    `json:"role"`
	CreatedAt   time.Time `json:"created_at"`
}

// ── Devices ─────────────────────────────────────────────────────────

type Device struct {
	ID          string     `json:"id"`
	DeviceKey   string     `json:"device_key"`
	DeviceName  string     `json:"device_name"`
	DeviceType  string     `json:"device_type"`
	OwnerID     *string    `json:"owner_id,omitempty"` // nil = unclaimed
	APIKey      string     `json:"-"`                  // never exposed in JSON
	IsActive    bool       `json:"is_active"`
	FirmwareVer string     `json:"firmware_ver,omitempty"`
	LastSeenAt  *time.Time `json:"last_seen_at,omitempty"`
	CreatedAt   time.Time  `json:"created_at"`
}

type ClaimDeviceRequest struct {
	APIKey string `json:"api_key"`
}

// ── Telemetry ───────────────────────────────────────────────────────

// TelemetryRow is the internal representation of one telemetry reading.
// The ingest worker builds this from the MQTT payload and passes it to
// the batch writer. Computed fields are enriched by the enricher.
type TelemetryRow struct {
	DeviceID   string
	DeviceType string
	Timestamp  time.Time
	RSSI       int8
	UptimeMS   uint32
	// Computed fields
	PVPower       float32
	BatteryPower  float32
	InverterPower float32
	DCLoadPower   float32
	SystemStatus  uint8
	MinSOCPct     float32
	MaxSOCPct     float32
	TotalEnergyWh float32
	// Raw device-specific fields
	Fields map[string]float64
}

// EnrichedTelemetry is the JSON payload republished to live/{device_key}.
// The API receives this and pushes it to WebSocket clients.
type EnrichedTelemetry struct {
	DeviceKey     string             `json:"device_key"`
	Timestamp     int64              `json:"ts"`
	TimestampMS   int                `json:"ts_ms"`
	Schema        string             `json:"schema"`
	FW            string             `json:"fw"`
	UptimeMS      uint32             `json:"uptime_ms"`
	RSSI          int8               `json:"rssi"`
	HeapFree      uint32             `json:"heap_free"`
	PVPower       float32            `json:"pv_power"`
	BatteryPower  float32            `json:"battery_power"`
	InverterPower float32            `json:"inverter_power"`
	DCLoadPower   float32            `json:"dc_load_power"`
	SystemStatus  uint8              `json:"system_status"`
	MinSOCPct     float32            `json:"min_soc_pct"`
	MaxSOCPct     float32            `json:"max_soc_pct"`
	TotalEnergyWh float32            `json:"total_energy_wh"`
	Fields        map[string]float64 `json:"fields"`
}

// ── Audit ───────────────────────────────────────────────────────────

type AuditEntry struct {
	ActorID      string         `json:"actor_id"`
	ActorType    string         `json:"actor_type"`    // 'user', 'device', 'system'
	Action       string         `json:"action"`        // e.g. 'device.claim', 'user.login'
	ResourceType string         `json:"resource_type"` // e.g. 'device', 'user'
	ResourceID   string         `json:"resource_id"`
	Details      map[string]any `json:"details,omitempty"`
	IPAddress    string         `json:"ip_address,omitempty"`
	UserAgent    string         `json:"user_agent,omitempty"`
}

// ── Standard API Envelopes ──────────────────────────────────────────

type APIResponse struct {
	Data any `json:"data,omitempty"`
}

type APIError struct {
	Error APIErrorDetail `json:"error"`
}

type APIErrorDetail struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	Field     string `json:"field,omitempty"`
	RequestID string `json:"request_id"`
}

type PaginatedResponse struct {
	Data       any        `json:"data"`
	Pagination Pagination `json:"pagination"`
}

type Pagination struct {
	Total   int  `json:"total"`
	Limit   int  `json:"limit"`
	Offset  int  `json:"offset"`
	HasMore bool `json:"has_more"`
}
