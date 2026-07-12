// internal/handlers.go — REST API handlers for the API server.
// All handlers are methods on Handlers, which holds shared dependencies.

package internal

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Handlers struct {
	pg  *pgxpool.Pool
	jwt *JWTManager
	ch  any // clickhouse.Conn — typed in real code
}

func NewHandlers(pg *pgxpool.Pool, jwt *JWTManager, ch any) *Handlers {
	return &Handlers{pg: pg, jwt: jwt, ch: ch}
}

// ── Auth ────────────────────────────────────────────────────────────

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

	access, _ := h.jwt.IssueAccessToken(user.ID, user.Role)
	refresh, _ := h.jwt.IssueRefreshToken(user.ID)
	writeJSON(w, http.StatusCreated, AuthResponse{
		AccessToken:  access,
		RefreshToken: refresh,
		User:         user,
	})
}

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

	access, _ := h.jwt.IssueAccessToken(user.ID, user.Role)
	refresh, _ := h.jwt.IssueRefreshToken(user.ID)
	writeJSON(w, http.StatusOK, AuthResponse{
		AccessToken:  access,
		RefreshToken: refresh,
		User:         user,
	})
}

func (h *Handlers) RefreshToken(w http.ResponseWriter, r *http.Request) {
	var req struct {
		RefreshToken string `json:"refresh_token"`
	}
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
	writeJSON(w, http.StatusOK, map[string]string{"status": "claimed"})
}

// ── Telemetry ───────────────────────────────────────────────────────

func (h *Handlers) GetLatestTelemetry(w http.ResponseWriter, r *http.Request) {
	deviceKey := chi.URLParam(r, "key")
	// Query ClickHouse for the latest row
	var ts time.Time
	var payload json.RawMessage
	err := h.pg.QueryRow(r.Context(),
		`SELECT recorded_at, payload FROM telemetry_live
		 WHERE device_id = $1 ORDER BY recorded_at DESC LIMIT 1`, deviceKey).Scan(&ts, &payload)
	if err == pgx.ErrNoRows {
		writeJSON(w, http.StatusOK, map[string]any{"device_key": deviceKey, "data": nil})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"device_key":  deviceKey,
		"recorded_at": ts,
		"data":        json.RawMessage(payload),
	})
}

// ── Health ──────────────────────────────────────────────────────────

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

func (h *Handlers) GetNotificationPrefs(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)
	var prefs struct {
		AlertFired    bool `json:"alert_fired_email"`
		AlertResolved bool `json:"alert_resolved_email"`
		QuietStart    *int `json:"quiet_hours_start,omitempty"`
		QuietEnd      *int `json:"quiet_hours_end,omitempty"`
	}
	err := h.pg.QueryRow(r.Context(),
		`SELECT alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end
		 FROM notification_preferences WHERE user_id = $1`, userID).
		Scan(&prefs.AlertFired, &prefs.AlertResolved, &prefs.QuietStart, &prefs.QuietEnd)
	if err != nil {
		writeJSON(w, http.StatusOK, prefs) // defaults
		return
	}
	writeJSON(w, http.StatusOK, prefs)
}

func (h *Handlers) UpdateNotificationPrefs(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)
	var req struct {
		AlertFired    *bool `json:"alert_fired_email"`
		AlertResolved *bool `json:"alert_resolved_email"`
		QuietStart    *int  `json:"quiet_hours_start"`
		QuietEnd      *int  `json:"quiet_hours_end"`
	}
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
