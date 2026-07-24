// internal/model.go — Shared data types used across both binaries.
// Every exported type has a doc comment explaining its purpose.

package internal

import "time"

// ── Auth ────────────────────────────────────────────────────────────

// RegisterRequest is the body of POST /auth/register.
type RegisterRequest struct {
	Email       string `json:"email"`
	Password    string `json:"password"`
	DisplayName string `json:"display_name,omitempty"`
}

// LoginRequest is the body of POST /auth/login.
type LoginRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

// AuthResponse is returned on successful login/register/refresh.
type AuthResponse struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	User         User   `json:"user"`
}

// User is the public representation of an authenticated account.
type User struct {
	ID          string    `json:"id"`
	Email       string    `json:"email"`
	DisplayName string    `json:"display_name,omitempty"`
	Role        string    `json:"role"`
	CreatedAt   time.Time `json:"created_at"`
}

// ── Devices ─────────────────────────────────────────────────────────

// Device is the API view of a provisioned hardware unit.
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

// ClaimDeviceRequest is the body of POST /devices/claim; it carries the
// device's pre-shared API key that authenticates the claim.
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
	// Computed fields are derived from raw Fields by the enricher; they are
	// stored as top-level columns in ClickHouse for fast filtering/aggregation.
	PVPower       float32
	BatteryPower  float32
	InverterPower float32
	DCLoadPower   float32
	SystemStatus  uint8
	MinSOCPct     float32
	MaxSOCPct     float32
	TotalEnergyWh float32
	// Raw device-specific fields are kept as a loose map so new firmware
	// schema versions don't require a column migration to persist.
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

// AuditEntry is the in-memory representation of one auditable action; it is
// persisted to audit_log by Auditor.Log.
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

// APIError is the top-level error envelope returned by all REST endpoints.
type APIError struct {
	Error APIErrorDetail `json:"error"`
}

// APIErrorDetail carries the machine-readable code, human message, optional
// offending field, and the request_id for cross-referencing logs.
type APIErrorDetail struct {
	Code      string `json:"code"`
	Message   string `json:"message"`
	Field     string `json:"field,omitempty"`
	RequestID string `json:"request_id"`
}

// ── Channel Groups ─────────────────────────────────────────────────

// ChannelGroup binds a user-facing label/icon to a set of telemetry channels
// via a bitmask.
type ChannelGroup struct {
	GroupID     int    `json:"group_id"`
	Name        string `json:"name"`
	Icon        int    `json:"icon"`         // 0=solar, 1=battery, 2=load, 3=generic
	ChannelMask int    `json:"channel_mask"` // bitmask: bit 0 = ch0, bit 1 = ch1, etc.
}

// EnrichmentResult is the set of derived columns the enricher computes from
// raw telemetry fields; it is merged into TelemetryRow/EnrichedTelemetry.
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

// RefreshRequest is the body of POST /auth/refresh.
type RefreshRequest struct {
	RefreshToken string `json:"refresh_token"`
}

// LatestTelemetry is the /devices/{key}/telemetry/latest response shape.
type LatestTelemetry struct {
	DeviceKey     string             `json:"device_key"`
	RecordedAt    time.Time          `json:"recorded_at"`
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

// HealthResponse is the body of GET /health; Services maps dependency names
// to their individual status (e.g. {"pg":"ok","clickhouse":"ok"}).
type HealthResponse struct {
	Status   string         `json:"status"`
	Services map[string]any `json:"services"`
}

// NotificationPrefs is a user's email-notification settings. QuietStart/QuietEnd
// are pointers so 0 (midnight) is distinguishable from "unset" (nil).
type NotificationPrefs struct {
	AlertFired    bool `json:"alert_fired_email"`
	AlertResolved bool `json:"alert_resolved_email"`
	QuietStart    *int `json:"quiet_hours_start,omitempty"`
	QuietEnd      *int `json:"quiet_hours_end,omitempty"`
}

// UpdateNotificationPrefsRequest is the PATCH body for notification prefs.
// Pointer fields mean "only update if present", allowing partial updates.
type UpdateNotificationPrefsRequest struct {
	AlertFired    *bool `json:"alert_fired_email"`
	AlertResolved *bool `json:"alert_resolved_email"`
	QuietStart    *int  `json:"quiet_hours_start"`
	QuietEnd      *int  `json:"quiet_hours_end"`
}

// Group is a logical grouping of devices for the dashboard.
type Group struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Description string `json:"description,omitempty"`
	Color       string `json:"color,omitempty"`
}

// Tags is the free-form key/value tag map attached to devices.
type Tags map[string]string

// SetTagRequest is the body of PUT /devices/{key}/tags/{name}.
type SetTagRequest struct {
	Value string `json:"value"`
}

// SearchResponse wraps full-text search hits plus the total match count for
// pagination.
type SearchResponse struct {
	Results []SearchResult `json:"results"`
	Total   int            `json:"total"`
	Query   string         `json:"query"`
}

// Invoice is the API view of a billing invoice. TotalCents is in minor units
// to avoid float rounding on monetary values.
type Invoice struct {
	ID            string    `json:"id"`
	InvoiceNumber string    `json:"invoice_number"`
	Description   string    `json:"description"`
	TotalCents    int       `json:"total_cents"`
	Status        string    `json:"status"`
	CreatedAt     time.Time `json:"created_at"`
}

// CreateInvoiceRequest is the admin body for creating an invoice.
type CreateInvoiceRequest struct {
	UserID      string `json:"user_id"`
	PlanID      int    `json:"plan_id"`
	Audience    string `json:"audience"`
	PeriodStart string `json:"period_start"`
	PeriodEnd   string `json:"period_end"`
	AmountCents int    `json:"amount_cents"`
	Description string `json:"description"`
}

// CreateGroupRequest is the body for creating a device group.
type CreateGroupRequest struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Color       string `json:"color"`
}

// CreateReleaseRequest is the admin body for publishing an OTA firmware release.
type CreateReleaseRequest struct {
	DeviceType string `json:"device_type"`
	Version    string `json:"version"`
	Channel    string `json:"channel"`
	BinaryPath string `json:"binary_path"`
	BinarySize int    `json:"binary_size"`
	SHA256     string `json:"sha256"`
	Changelog  string `json:"changelog"`
}

// ExportDownloadResponse is returned by the export endpoint with a
// pre-signed URL and how many seconds it remains valid.
type ExportDownloadResponse struct {
	DownloadURL      string `json:"download_url"`
	ExpiresInSeconds int    `json:"expires_in_seconds"`
}

// ── License Plans ──────────────────────────────────────────────────

// LicensePlan describes a billing tier and its device/retention limits.
type LicensePlan struct {
	ID            int      `json:"id"`
	Name          string   `json:"name"`
	Audience      string   `json:"audience"`
	MaxDevices    int      `json:"max_devices"`
	RetentionDays int      `json:"retention_days"`
	Features      []string `json:"features"`
	PriceMonthly  int      `json:"price_monthly"`
}

// UserLicense is a user's current subscription; ExpiresAt is nil for
// non-expiring plans.
type UserLicense struct {
	UserID      string     `json:"user_id"`
	PlanID      int        `json:"plan_id"`
	DeviceCount int        `json:"device_count"`
	StartsAt    time.Time  `json:"starts_at"`
	ExpiresAt   *time.Time `json:"expires_at,omitempty"`
	UpdatedAt   time.Time  `json:"updated_at"`
}

// ── Device Commands ───────────────────────────────────────────────

// CreateCommandRequest is the body for enqueueing a command for a device to
// pull via MQTT.
type CreateCommandRequest struct {
	DeviceKey string         `json:"device_key"`
	CmdType   string         `json:"cmd_type"`
	Payload   map[string]any `json:"payload"`
}

// CommandResultRequest is posted by a device to report command outcome.
type CommandResultRequest struct {
	Status string         `json:"status"` // applied / failed
	Result map[string]any `json:"result,omitempty"`
	Error  string         `json:"error,omitempty"`
}

// DeviceCommand is the stored view of a queued command and its outcome.
// AppliedAt is nil until the device reports a result.
type DeviceCommand struct {
	ID        int64          `json:"id"`
	DeviceKey string         `json:"device_key"`
	CmdType   string         `json:"cmd_type"`
	Payload   map[string]any `json:"payload"`
	Status    string         `json:"status"`
	Result    map[string]any `json:"result,omitempty"`
	Error     string         `json:"error,omitempty"`
	CreatedAt time.Time      `json:"created_at"`
	AppliedAt *time.Time     `json:"applied_at,omitempty"`
}

// ── WebSocket Events ───────────────────────────────────────────────

// DeviceStatusEvent is pushed to WebSocket subscribers when a device's
// online/offline state changes.
type DeviceStatusEvent struct {
	Type      string `json:"type"`
	DeviceKey string `json:"device_key"`
	Online    bool   `json:"online"`
	Timestamp int64  `json:"ts"`
}

// MaintenanceToggleRequest is the body of POST /admin/maintenance.
type MaintenanceToggleRequest struct {
	Enabled bool   `json:"enabled"`
	Message string `json:"message"`
}

// MQTTAuthRequest is the body posted to the mosquitto auth plugin to
// authenticate a connecting device.
type MQTTAuthRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

// MQTTAuthResponse is returned by the mosquitto auth endpoint; ACLs is empty
// when the broker should deny all topics.
type MQTTAuthResponse struct {
	OK   bool      `json:"ok"`
	ACLs []MQTTACL `json:"acls,omitempty"`
}

// MQTTACL is one topic/access pair granted to an authenticated device.
type MQTTACL struct {
	Topic  string `json:"topic"`
	Access string `json:"access"`
}
