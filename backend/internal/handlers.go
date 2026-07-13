// internal/handlers.go — REST API handlers for the API server.
// All handlers are methods on Handlers, which holds shared dependencies.

// @title           IoT Platform API
// @version         1.0.0
// @description     Self-hosted IoT platform backend. Two binaries: api (HTTP/WS) and ingest (MQTT).
// @termsOfService  http://swagger.io/terms/

// @contact.name   IoT Platform Team
// @contact.email  dev@iotplatform.local

// @license.name  MIT
// @license.url   https://opensource.org/licenses/MIT

// @host      localhost:8080
// @BasePath  /api/v1

// @securityDefinitions.apikey  BearerAuth
// @in                          header
// @name                        Authorization
// @description                 JWT access token from /auth/login or /auth/register

package internal

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Handlers struct {
	pg      *pgxpool.Pool
	jwt     *JWTManager
	ch      clickhouse.Conn
	auditor *Auditor
}

func NewHandlers(pg *pgxpool.Pool, jwt *JWTManager, ch clickhouse.Conn) *Handlers {
	return &Handlers{pg: pg, jwt: jwt, ch: ch, auditor: NewAuditor(pg)}
}

// ── Auth ────────────────────────────────────────────────────────────

// Register creates a new user account
// @Summary      Register a new user
// @Tags         Auth
// @Accept       json
// @Produce      json
// @Param        body  body  RegisterRequest  true  "Registration details"
// @Success      201   {object}  AuthResponse
// @Failure      400   {object}  APIError
// @Failure      409   {object}  APIError
// @Router       /auth/register [post]
func (h *Handlers) Register(w http.ResponseWriter, r *http.Request) {
	var req RegisterRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}
	if req.Email == "" || req.Password == "" {
		writeError(w, "validation_error", "email and password are required", http.StatusBadRequest)
		return
	}
	if len(req.Password) < 8 {
		writeError(w, "validation_error", "password must be at least 8 characters", http.StatusBadRequest)
		return
	}

	hash, err := HashPassword(req.Password)
	if err != nil {
		writeError(w, "internal_error", "failed to hash password", http.StatusInternalServerError)
		return
	}

	var user User
	err = h.pg.QueryRow(r.Context(),
		`INSERT INTO users (email, password_hash, display_name)
		 VALUES ($1, $2, $3)
		 RETURNING id, email, display_name, role, created_at`,
		req.Email, hash, req.DisplayName).Scan(
		&user.ID, &user.Email, &user.DisplayName, &user.Role, &user.CreatedAt)
	if err != nil {
		if pgErr := err.Error(); contains(pgErr, "unique") || contains(pgErr, "duplicate") {
			writeError(w, "conflict", "email already registered", http.StatusConflict)
			return
		}
		writeError(w, "internal_error", "failed to create user", http.StatusInternalServerError)
		return
	}

	h.auditor.Log(r.Context(), AuditEntry{
		ActorID:      user.ID,
		ActorType:    "user",
		Action:       "user.register",
		ResourceType: "user",
		ResourceID:   user.ID,
		IPAddress:    r.RemoteAddr,
		UserAgent:    r.UserAgent(),
	})

	access, _ := h.jwt.IssueAccessToken(user.ID, user.Role)
	refresh, _ := h.jwt.IssueRefreshToken(user.ID)
	writeJSON(w, http.StatusCreated, AuthResponse{
		AccessToken:  access,
		RefreshToken: refresh,
		User:         user,
	})
}

// Login authenticates a user and returns JWT tokens
// @Summary      Login
// @Tags         Auth
// @Accept       json
// @Produce      json
// @Param        body  body  LoginRequest  true  "Login credentials"
// @Success      200   {object}  AuthResponse
// @Failure      401   {object}  APIError
// @Router       /auth/login [post]
func (h *Handlers) Login(w http.ResponseWriter, r *http.Request) {
	var req LoginRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}

	var user User
	var hash string
	err := h.pg.QueryRow(r.Context(),
		`SELECT id, email, password_hash, display_name, role, created_at
		 FROM users WHERE email = $1`,
		req.Email).Scan(&user.ID, &user.Email, &hash, &user.DisplayName, &user.Role, &user.CreatedAt)
	if err == pgx.ErrNoRows {
		writeError(w, "unauthorized", "invalid email or password", http.StatusUnauthorized)
		return
	}
	if err != nil {
		writeError(w, "internal_error", "database error", http.StatusInternalServerError)
		return
	}

	if !CheckPassword(hash, req.Password) {
		writeError(w, "unauthorized", "invalid email or password", http.StatusUnauthorized)
		return
	}

	h.auditor.Log(r.Context(), AuditEntry{
		ActorID:      user.ID,
		ActorType:    "user",
		Action:       "user.login",
		ResourceType: "user",
		ResourceID:   user.ID,
		IPAddress:    r.RemoteAddr,
		UserAgent:    r.UserAgent(),
	})

	access, _ := h.jwt.IssueAccessToken(user.ID, user.Role)
	refresh, _ := h.jwt.IssueRefreshToken(user.ID)
	writeJSON(w, http.StatusOK, AuthResponse{
		AccessToken:  access,
		RefreshToken: refresh,
		User:         user,
	})
}

// RefreshToken returns new access and refresh tokens
// @Summary      Refresh JWT tokens
// @Tags         Auth
// @Accept       json
// @Produce      json
// @Param        body  body  RefreshRequest  true  "Refresh token"
// @Success      200   {object}  map[string]string
// @Failure      401   {object}  APIError
// @Router       /auth/refresh [post]
func (h *Handlers) RefreshToken(w http.ResponseWriter, r *http.Request) {
	var req RefreshRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}

	claims, err := h.jwt.ValidateToken(req.RefreshToken)
	if err != nil {
		writeError(w, "unauthorized", "invalid or expired refresh token", http.StatusUnauthorized)
		return
	}

	access, _ := h.jwt.IssueAccessToken(claims.UserID, claims.Role)
	refresh, _ := h.jwt.IssueRefreshToken(claims.UserID)
	writeJSON(w, http.StatusOK, map[string]string{
		"access_token":  access,
		"refresh_token": refresh,
	})
}

// ── Devices ─────────────────────────────────────────────────────────

// ListDevices returns all devices owned by the authenticated user
// @Summary      List user's devices
// @Tags         Devices
// @Produce      json
// @Success      200  {array}  Device
// @Security     BearerAuth
// @Router       /devices [get]
func (h *Handlers) ListDevices(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)

	rows, err := h.pg.Query(r.Context(),
		`SELECT id, device_key, device_name, device_type, owner_id::text, is_active,
		        coalesce(firmware_ver, ''), last_seen_at, created_at
		 FROM devices WHERE owner_id = $1
		 ORDER BY created_at DESC LIMIT 100`, userID)
	if err != nil {
		writeError(w, "internal_error", "failed to query devices", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	devices := []Device{}
	for rows.Next() {
		var d Device
		rows.Scan(&d.ID, &d.DeviceKey, &d.DeviceName, &d.DeviceType, &d.OwnerID,
			&d.IsActive, &d.FirmwareVer, &d.LastSeenAt, &d.CreatedAt)
		devices = append(devices, d)
	}
	writeJSON(w, http.StatusOK, devices)
}

// GetDevice returns a single device by device_key
// @Summary      Get device details
// @Tags         Devices
// @Produce      json
// @Param        key  path  string  true  "Device key"
// @Success      200  {object}  Device
// @Failure      404  {object}  APIError
// @Security     BearerAuth
// @Router       /devices/{key} [get]
func (h *Handlers) GetDevice(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	userID := r.Context().Value(ContextUserID).(string)

	var d Device
	err := h.pg.QueryRow(r.Context(),
		`SELECT id, device_key, device_name, device_type, owner_id::text, is_active,
		        coalesce(firmware_ver, ''), last_seen_at, created_at
		 FROM devices WHERE device_key = $1 AND owner_id = $2`,
		deviceKey, userID).Scan(
		&d.ID, &d.DeviceKey, &d.DeviceName, &d.DeviceType, &d.OwnerID,
		&d.IsActive, &d.FirmwareVer, &d.LastSeenAt, &d.CreatedAt)
	if err == pgx.ErrNoRows {
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}
	if err != nil {
		writeError(w, "internal_error", "database error", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, d)
}

// ClaimDevice claims an unclaimed device using its API key
// @Summary      Claim an unclaimed device
// @Tags         Devices
// @Accept       json
// @Produce      json
// @Param        key   path  string              true  "Device key"
// @Param        body  body  ClaimDeviceRequest  true  "Device API key"
// @Success      200   {object}  map[string]string
// @Failure      404   {object}  APIError
// @Security     BearerAuth
// @Router       /devices/{key}/claim [post]
func (h *Handlers) ClaimDevice(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	userID := r.Context().Value(ContextUserID).(string)

	var req ClaimDeviceRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}

	tag, err := h.pg.Exec(r.Context(),
		`UPDATE devices SET owner_id = $1, device_name = 'My ' || device_type
		 WHERE device_key = $2 AND owner_id IS NULL AND api_key::text = $3`,
		userID, deviceKey, req.APIKey)
	if err != nil {
		writeError(w, "internal_error", "failed to claim device", http.StatusInternalServerError)
		return
	}
	if tag.RowsAffected() == 0 {
		writeError(w, "not_found", "device not found or already claimed", http.StatusNotFound)
		return
	}

	h.auditor.Log(r.Context(), AuditEntry{
		ActorID:      userID,
		ActorType:    "user",
		Action:       "device.claim",
		ResourceType: "device",
		ResourceID:   deviceKey,
		IPAddress:    r.RemoteAddr,
		UserAgent:    r.UserAgent(),
	})

	writeJSON(w, http.StatusOK, map[string]string{"status": "claimed"})
}

// ── Telemetry ───────────────────────────────────────────────────────

// GetLatestTelemetry returns the most recent telemetry reading from ClickHouse
// @Summary      Get latest telemetry
// @Tags         Telemetry
// @Produce      json
// @Param        key  path  string  true  "Device key"
// @Success      200  {object}  LatestTelemetry
// @Security     BearerAuth
// @Router       /telemetry/{key}/latest [get]
func (h *Handlers) GetLatestTelemetry(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	if h.ch == nil {
		writeJSON(w, http.StatusOK, map[string]any{"device_key": deviceKey, "data": nil})
		return
	}
	rows, err := h.ch.Query(r.Context(),
		`SELECT ts, pv_power, battery_power, inverter_power, dc_load_power,
		        system_status, min_soc_pct, max_soc_pct, total_energy_wh, fields
		 FROM device_telemetry
		 WHERE device_id = $1 ORDER BY ts DESC LIMIT 1`, deviceKey)
	if err != nil {
		writeJSON(w, http.StatusOK, map[string]any{"device_key": deviceKey, "data": nil})
		return
	}
	defer rows.Close()

	if !rows.Next() {
		writeJSON(w, http.StatusOK, map[string]any{"device_key": deviceKey, "data": nil})
		return
	}
	var ts time.Time
	var pv, bat, inv, dc float32
	var status uint8
	var minSoc, maxSoc, energy float32
	var fields map[string]float64
	rows.Scan(&ts, &pv, &bat, &inv, &dc, &status, &minSoc, &maxSoc, &energy, &fields)
	writeJSON(w, http.StatusOK, map[string]any{
		"device_key":      deviceKey,
		"recorded_at":     ts,
		"pv_power":        pv,
		"battery_power":   bat,
		"inverter_power":  inv,
		"dc_load_power":   dc,
		"system_status":   status,
		"min_soc_pct":     minSoc,
		"max_soc_pct":     maxSoc,
		"total_energy_wh": energy,
		"fields":          fields,
	})
}

// ── Health ──────────────────────────────────────────────────────────

// Health returns service health status
// @Summary      Health check
// @Tags         Monitoring
// @Produce      json
// @Success      200  {object}  HealthResponse
// @Router       /health [get]
func (h *Handlers) Health(w http.ResponseWriter, r *http.Request) {
	services := map[string]any{}

	if err := h.pg.Ping(r.Context()); err != nil {
		services["postgres"] = map[string]any{"status": "down", "error": err.Error()}
	} else {
		services["postgres"] = map[string]any{"status": "ok"}
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"status":   "ok",
		"services": services,
	})
}

// ── Notification Preferences ────────────────────────────────────────

// GetNotificationPrefs returns the authenticated user's notification preferences
// @Summary      Get notification preferences
// @Tags         Notifications
// @Produce      json
// @Success      200  {object}  NotificationPrefs
// @Security     BearerAuth
// @Router       /users/me/notifications [get]
func (h *Handlers) GetNotificationPrefs(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)
	var prefs NotificationPrefs
	err := h.pg.QueryRow(r.Context(),
		`SELECT alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end
		 FROM notification_preferences WHERE user_id = $1`, userID).
		Scan(&prefs.AlertFired, &prefs.AlertResolved, &prefs.QuietStart, &prefs.QuietEnd)
	if err != nil {
		writeJSON(w, http.StatusOK, prefs)
		return
	}
	writeJSON(w, http.StatusOK, prefs)
}

// UpdateNotificationPrefs updates the authenticated user's notification preferences
// @Summary      Update notification preferences
// @Tags         Notifications
// @Accept       json
// @Produce      json
// @Param        body  body  UpdateNotificationPrefsRequest  true  "Preferences"
// @Success      200   {object}  map[string]string
// @Security     BearerAuth
// @Router       /users/me/notifications [patch]
func (h *Handlers) UpdateNotificationPrefs(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)
	var req UpdateNotificationPrefsRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
		return
	}
	h.pg.Exec(r.Context(), `
		INSERT INTO notification_preferences (user_id, alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end)
		VALUES ($1, $2, $3, $4, $5)
		ON CONFLICT (user_id) DO UPDATE SET
		    alert_fired_email = COALESCE($2, notification_preferences.alert_fired_email),
		    alert_resolved_email = COALESCE($3, notification_preferences.alert_resolved_email),
		    quiet_hours_start = $4,
		    quiet_hours_end = $5,
		    updated_at = now()`,
		userID, req.AlertFired, req.AlertResolved, req.QuietStart, req.QuietEnd)
	writeJSON(w, http.StatusOK, map[string]string{"status": "updated"})
}

// ── Helpers ─────────────────────────────────────────────────────────

func contains(s, substr string) bool {
	return len(s) >= len(substr) && searchString(s, substr)
}

func searchString(s, substr string) bool {
	for i := 0; i <= len(s)-len(substr); i++ {
		if s[i:i+len(substr)] == substr {
			return true
		}
	}
	return false
}
