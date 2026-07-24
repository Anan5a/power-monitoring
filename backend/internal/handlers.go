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
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Handlers holds shared dependencies (Postgres pool, JWT manager, ClickHouse
// connection, audit logger, license checker, refresh-token store) for the
// REST API handlers. All handler methods are defined on this type.
type Handlers struct {
	pg       *pgxpool.Pool
	jwt      *JWTManager
	ch       clickhouse.Conn
	auditor  *Auditor
	licenses *LicenseChecker
	refresh  *RefreshTokenStore
}

// NewHandlers constructs a Handlers value wiring the supplied dependencies.
// The Auditor and LicenseChecker are derived from the Postgres pool internally
// so callers only need to provide the pool, JWT manager, ClickHouse connection,
// and refresh-token store.
func NewHandlers(pg *pgxpool.Pool, jwt *JWTManager, ch clickhouse.Conn, refresh *RefreshTokenStore) *Handlers {
	return &Handlers{pg: pg, jwt: jwt, ch: ch, auditor: NewAuditor(pg), licenses: NewLicenseChecker(pg), refresh: refresh}
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
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == "23505" { // unique_violation
			writeError(w, "conflict", "email already registered", http.StatusConflict)
			return
		}
		writeError(w, "internal_error", "failed to create user", http.StatusInternalServerError)
		return
	}

	// Record the registration in the audit log for compliance/tracing.
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "user.register",
		ResourceType: "user",
		ResourceID:   user.ID,
	})

	access, err := h.jwt.IssueAccessToken(user.ID, user.Role)
	if err != nil {
		writeError(w, "internal_error", "failed to issue access token", http.StatusInternalServerError)
		return
	}
	refresh, err := h.refresh.Issue(r.Context(), user.ID)
	if err != nil {
		writeError(w, "internal_error", "failed to issue refresh token", http.StatusInternalServerError)
		return
	}
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
		// Run a dummy bcrypt comparison against a fixed hash so the missing-user
		// path takes roughly the same time as the existing-user path, avoiding a
		// timing side-channel that would let an attacker enumerate accounts.
		_ = CheckPassword(DummyBcryptHash(), req.Password)
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

	// Audit successful authentication.
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "user.login",
		ResourceType: "user",
		ResourceID:   user.ID,
	})

	access, err := h.jwt.IssueAccessToken(user.ID, user.Role)
	if err != nil {
		writeError(w, "internal_error", "failed to issue access token", http.StatusInternalServerError)
		return
	}
	refresh, err := h.refresh.Issue(r.Context(), user.ID)
	if err != nil {
		writeError(w, "internal_error", "failed to issue refresh token", http.StatusInternalServerError)
		return
	}
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

	// Rotate the refresh token: the old token is revoked and a new one is
	// issued in the same family. Reuse of a revoked token revokes the family.
	userID, newRefresh, err := h.refresh.Rotate(r.Context(), req.RefreshToken)
	if err != nil {
		switch {
		case err == ErrRefreshTokenReuse:
			// Reuse attack — do not reveal details.
			writeError(w, "unauthorized", "invalid or expired refresh token", http.StatusUnauthorized)
		case err == ErrRefreshTokenNotFound:
			writeError(w, "unauthorized", "invalid or expired refresh token", http.StatusUnauthorized)
		default:
			// Includes expired-token errors from ValidateToken.
			writeError(w, "unauthorized", "invalid or expired refresh token", http.StatusUnauthorized)
		}
		return
	}

	// Look up the user's current role so the new access token reflects role
	// changes (e.g. promotion/demotion) rather than a stale claim.
	var role string
	if err := h.pg.QueryRow(r.Context(),
		`SELECT role FROM users WHERE id = $1`, userID).Scan(&role); err != nil {
		writeError(w, "unauthorized", "user no longer exists", http.StatusUnauthorized)
		return
	}

	access, err := h.jwt.IssueAccessToken(userID, role)
	if err != nil {
		writeError(w, "internal_error", "failed to issue access token", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{
		"access_token":  access,
		"refresh_token": newRefresh,
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	rows, err := h.pg.Query(r.Context(),
		`SELECT id, device_key, device_name, device_type, owner_id::text, is_active,
		        coalesce(firmware_ver, ''), last_seen_at, created_at
		 FROM devices WHERE owner_id = $1
		 ORDER BY created_at DESC LIMIT 100`, userID)
	// The owner_id filter enforces tenancy: a user can only ever see their own devices.
	if err != nil {
		writeError(w, "internal_error", "failed to query devices", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	devices := []Device{}
	for rows.Next() {
		var d Device
		if err := rows.Scan(&d.ID, &d.DeviceKey, &d.DeviceName, &d.DeviceType, &d.OwnerID,
			&d.IsActive, &d.FirmwareVer, &d.LastSeenAt, &d.CreatedAt); err != nil {
			writeError(w, "internal_error", "failed to read devices", http.StatusInternalServerError)
			return
		}
		devices = append(devices, d)
	}
	if err := rows.Err(); err != nil {
		writeError(w, "internal_error", "failed to read devices", http.StatusInternalServerError)
		return
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	var d Device
	err := h.pg.QueryRow(r.Context(),
		`SELECT id, device_key, device_name, device_type, owner_id::text, is_active,
		        coalesce(firmware_ver, ''), last_seen_at, created_at
		 FROM devices WHERE device_key = $1 AND owner_id = $2`,
		deviceKey, userID).Scan(
		// owner_id = $2 is the ownership check: a non-owner gets the same 404
		// response as a missing device, avoiding disclosure of device existence.
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	var req ClaimDeviceRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
		return
	}

	plan, err := h.licenses.ClaimDevice(r.Context(), userID, deviceKey, req.APIKey)
	if err != nil {
		switch {
		case err == ErrLicenseCapReached:
			msg := "device limit reached"
			if plan != nil {
				msg = fmt.Sprintf("device limit reached for %s plan (max %d devices)", plan.Name, plan.MaxDevices)
			}
			writeError(w, "forbidden", msg, http.StatusForbidden)
		case err == ErrDeviceAlreadyClaimed:
			writeError(w, "not_found", "device not found or already claimed", http.StatusNotFound)
		default:
			writeError(w, "internal_error", "failed to claim device", http.StatusInternalServerError)
		}
		return
	}

	// Audit the claim so ownership transitions are traceable.
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "device.claim",
		ResourceType: "device",
		ResourceID:   deviceKey,
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	if !IsDeviceOwner(r.Context(), h.pg, deviceKey, userID) {
		// Ownership gate: non-owners receive a 404 rather than a 403 to avoid
		// leaking the existence of devices they do not own.
		writeError(w, "not_found", "device not found", http.StatusNotFound)
		return
	}
	if h.ch == nil {
		// ClickHouse not configured — return a payload with null data rather
		// than erroring, so the API still works in a degraded telemetry-less mode.
		writeJSON(w, http.StatusOK, map[string]any{"device_key": deviceKey, "data": nil})
		return
	}
	rows, err := h.ch.Query(r.Context(),
		`SELECT ts, pv_power, battery_power, inverter_power, dc_load_power,
		        system_status, min_soc_pct, max_soc_pct, total_energy_wh, fields
		 FROM device_telemetry
		 WHERE device_id = ? ORDER BY ts DESC LIMIT 1`, deviceKey)
	// ClickHouse uses positional "?" placeholders, unlike Postgres's "$n" style.
	if err != nil {
		// On query error degrade gracefully to null data instead of surfacing
		// the internal ClickHouse failure to the client.
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
	// rows.Scan error is intentionally ignored: a scan mismatch yields zero
	// values and a null-ish payload rather than a 500 for this read-only path.
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

	// nil pg/ch means the dependency was not wired at startup; report "unknown"
	// rather than panicking so health remains callable in partial deployments.
	if h.pg == nil {
		services["postgres"] = map[string]any{"status": "unknown"}
	} else if err := h.pg.Ping(r.Context()); err != nil {
		services["postgres"] = map[string]any{"status": "down", "error": err.Error()}
	} else {
		services["postgres"] = map[string]any{"status": "ok"}
	}

	if h.ch == nil {
		services["clickhouse"] = map[string]any{"status": "unknown"}
	} else if err := h.ch.Ping(r.Context()); err != nil {
		services["clickhouse"] = map[string]any{"status": "down", "error": err.Error()}
	} else {
		services["clickhouse"] = map[string]any{"status": "ok"}
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	var prefs NotificationPrefs
	err := h.pg.QueryRow(r.Context(),
		`SELECT alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end
		 FROM notification_preferences WHERE user_id = $1`, userID).
		Scan(&prefs.AlertFired, &prefs.AlertResolved, &prefs.QuietStart, &prefs.QuietEnd)
	if err != nil {
		// No row yet (or scan error): return zero-value prefs so callers get a
		// stable default rather than a 404 for a missing-preferences row.
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	var req UpdateNotificationPrefsRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
		return
	}
	if _, err := h.pg.Exec(r.Context(), `
		INSERT INTO notification_preferences (user_id, alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end)
		VALUES ($1, $2, $3, $4, $5)
		ON CONFLICT (user_id) DO UPDATE SET
		    alert_fired_email = COALESCE($2, notification_preferences.alert_fired_email),
		    alert_resolved_email = COALESCE($3, notification_preferences.alert_resolved_email),
		    quiet_hours_start = $4,
		    quiet_hours_end = $5,
		    updated_at = now()`,
		userID, req.AlertFired, req.AlertResolved, req.QuietStart, req.QuietEnd); err != nil {
		// Upsert: COALESCE preserves the existing email flags when the request
		// omits them (NULL), while quiet-hours are overwritten unconditionally.
		writeError(w, "internal_error", "failed to update preferences", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "updated"})
}

// ListPlans returns all available license plans
// @Summary      List license plans
// @Tags         Billing
// @Produce      json
// @Success      200  {array}  LicensePlan
// @Router       /billing/plans [get]
func (h *Handlers) ListPlans(w http.ResponseWriter, r *http.Request) {
	plans, err := h.licenses.ListPlans(r.Context())
	if err != nil {
		writeError(w, "internal_error", "failed to list plans", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, plans)
}

// ── Audit ────────────────────────────────────────────────────────────

// ListAudit returns audit log entries for the authenticated user.
// Admins see all entries.
// @Summary      Query audit log
// @Tags         Admin
// @Produce      json
// @Param        action        query  string  false  "Filter by action"
// @Param        resource_type query  string  false  "Filter by resource type"
// @Param        limit         query  int     false  "Max results"  default(50)
// @Param        offset        query  int     false  "Result offset"  default(0)
// @Success      200  {array}  AuditEntry
// @Security     BearerAuth
// @Router       /admin/audit [get]
func (h *Handlers) ListAudit(w http.ResponseWriter, r *http.Request) {
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	role, _ := r.Context().Value(ContextUserRole).(string)

	action := r.URL.Query().Get("action")
	resourceType := r.URL.Query().Get("resource_type")
	limit := parseInt(r.URL.Query().Get("limit"), 50)
	offset := parseInt(r.URL.Query().Get("offset"), 0)

	query := `
		SELECT actor_id::text, actor_type, action, resource_type, resource_id, details, ip_address, user_agent, created_at
		FROM audit_log`
	args := []any{}
	where := []string{}
	// Non-admins are restricted to their own audit entries via actor_id; admins
	// skip this filter and see global entries.
	if role != "admin" {
		args = append(args, userID)
		where = append(where, fmt.Sprintf("actor_id = $%d", len(args)))
	}
	// Filters are appended conditionally; placeholder numbering ($1, $2, ...)
	// is derived from the running args slice length so it always matches the
	// parameter order regardless of which filters are applied.
	if action != "" {
		args = append(args, action)
		where = append(where, fmt.Sprintf("action = $%d", len(args)))
	}
	if resourceType != "" {
		args = append(args, resourceType)
		where = append(where, fmt.Sprintf("resource_type = $%d", len(args)))
	}
	if len(where) > 0 {
		query += " WHERE " + strings.Join(where, " AND ")
	}
	// LIMIT/OFFSET are the last two params; their placeholder indices are the
	// final two positions of the args slice.
	args = append(args, limit, offset)
	query += fmt.Sprintf(" ORDER BY created_at DESC LIMIT $%d OFFSET $%d", len(args)-1, len(args))

	rows, err := h.pg.Query(r.Context(), query, args...)
	if err != nil {
		writeError(w, "internal_error", "failed to query audit log", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	entries := []AuditEntry{}
	for rows.Next() {
		var e AuditEntry
		var details []byte
		var createdAt time.Time
		rows.Scan(&e.ActorID, &e.ActorType, &e.Action, &e.ResourceType, &e.ResourceID,
			&details, &e.IPAddress, &e.UserAgent, &createdAt)
		// Ensure Details is a non-nil map before unmarshal so json.Unmarshal
		// populates it in place; nil would leave the field nil after unmarshal.
		if e.Details == nil {
			e.Details = map[string]any{}
		}
		json.Unmarshal(details, &e.Details)
		// After unmarshal Details may still be nil if the column was NULL or
		// held JSON null — normalize back to an empty object for stable JSON.
		if e.Details == nil {
			e.Details = map[string]any{}
		}
		e.Details["created_at"] = createdAt
		entries = append(entries, e)
	}
	if err := rows.Err(); err != nil {
		writeError(w, "internal_error", "failed to read audit log", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, entries)
}

// ── Helpers ─────────────────────────────────────────────────────────
