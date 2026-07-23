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
	DeviceType    string             `json:"device_type"`
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

// ── Channel Groups ─────────────────────────────────────────────────

type ChannelGroup struct {
	GroupID     int    `json:"group_id"`
	Name        string `json:"name"`
	Icon        int    `json:"icon"`        // 0=solar, 1=battery, 2=load, 3=generic
	ChannelMask int    `json:"channel_mask"` // bitmask: bit 0 = ch0, bit 1 = ch1, etc.
}

type EnrichmentResult struct {
	PVPower       float32
	BatteryPower  float32
	InverterPower float32
	DCLoadPower   float32
	SystemStatus  uint8
	MinSOCPct     float32
	MaxSOCPct     float32
	TotalEnergyWh float32
}

// ── Swagger types (used by swaggo annotations) ─────────────────────

type RefreshRequest struct {
	RefreshToken string `json:"refresh_token"`
}

type LatestTelemetry struct {
	DeviceKey      string             `json:"device_key"`
	RecordedAt     time.Time          `json:"recorded_at"`
	PVPower        float32            `json:"pv_power"`
	BatteryPower   float32            `json:"battery_power"`
	InverterPower  float32            `json:"inverter_power"`
	DCLoadPower    float32            `json:"dc_load_power"`
	SystemStatus   uint8              `json:"system_status"`
	MinSOCPct      float32            `json:"min_soc_pct"`
	MaxSOCPct      float32            `json:"max_soc_pct"`
	TotalEnergyWh  float32            `json:"total_energy_wh"`
	Fields         map[string]float64 `json:"fields"`
}

type HealthResponse struct {
	Status   string         `json:"status"`
	Services map[string]any `json:"services"`
}

type NotificationPrefs struct {
	AlertFired    bool `json:"alert_fired_email"`
	AlertResolved bool `json:"alert_resolved_email"`
	QuietStart    *int `json:"quiet_hours_start,omitempty"`
	QuietEnd      *int `json:"quiet_hours_end,omitempty"`
}

type UpdateNotificationPrefsRequest struct {
	AlertFired    *bool `json:"alert_fired_email"`
	AlertResolved *bool `json:"alert_resolved_email"`
	QuietStart    *int  `json:"quiet_hours_start"`
	QuietEnd      *int  `json:"quiet_hours_end"`
}

type Group struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Description string `json:"description,omitempty"`
	Color       string `json:"color,omitempty"`
}

type Tags map[string]string

type SetTagRequest struct {
	Value string `json:"value"`
}

type SearchResponse struct {
	Results []SearchResult `json:"results"`
	Total   int            `json:"total"`
	Query   string         `json:"query"`
}

type Invoice struct {
	ID            string    `json:"id"`
	InvoiceNumber string    `json:"invoice_number"`
	Description   string    `json:"description"`
	TotalCents    int       `json:"total_cents"`
	Status        string    `json:"status"`
	CreatedAt     time.Time `json:"created_at"`
}

type CreateInvoiceRequest struct {
	UserID      string `json:"user_id"`
	PlanID      int    `json:"plan_id"`
	Audience    string `json:"audience"`
	PeriodStart string `json:"period_start"`
	PeriodEnd   string `json:"period_end"`
	AmountCents int    `json:"amount_cents"`
	Description string `json:"description"`
}

type CreateGroupRequest struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Color       string `json:"color"`
}

type CreateReleaseRequest struct {
	DeviceType string `json:"device_type"`
	Version    string `json:"version"`
	Channel    string `json:"channel"`
	BinaryPath string `json:"binary_path"`
	BinarySize int    `json:"binary_size"`
	SHA256     string `json:"sha256"`
	Changelog  string `json:"changelog"`
}

type ExportRequestResponse struct {
	JobID string `json:"job_id"`
}

type ExportStatusResponse struct {
	Status      string     `json:"status"`
	FilePath    string     `json:"file_path"`
	CompletedAt *time.Time `json:"completed_at"`
}

type MaintenanceToggleRequest struct {
	Enabled bool   `json:"enabled"`
	Message string `json:"message"`
}

type MQTTAuthRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type MQTTAuthResponse struct {
	OK   bool      `json:"ok"`
	ACLs []MQTTACL `json:"acls,omitempty"`
}

type MQTTACL struct {
	Topic  string `json:"topic"`
	Access string `json:"access"`
}
