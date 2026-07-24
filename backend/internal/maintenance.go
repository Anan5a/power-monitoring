// internal/maintenance.go — Maintenance mode toggle and middleware.
// When enabled, all endpoints (except /health) return 503.
// The ingest worker also checks the flag and pauses processing.

package internal

import (
	"context"
	"encoding/json"
	"net/http"
	"sync"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// MaintenanceMode gates request handling on a DB-backed flag. It caches the
// flag in memory and refreshes it periodically so the hot path (Middleware)
// does not query the database on every request.
type MaintenanceMode struct {
	pg        *pgxpool.Pool
	auditor   *Auditor
	mu        sync.RWMutex
	enabled   bool
	message   string
	lastCheck time.Time
}

// NewMaintenanceMode constructs a MaintenanceMode and eagerly loads the
// current flag value so the first request sees a correct state.
func NewMaintenanceMode(pg *pgxpool.Pool) *MaintenanceMode {
	m := &MaintenanceMode{pg: pg, auditor: NewAuditor(pg)}
	m.refresh()
	return m
}

// refresh reloads the enabled/message pair from the database under a write
// lock. Errors are swallowed to avoid taking the whole API down if the DB
// blips; the previously cached value remains in effect.
func (m *MaintenanceMode) refresh() {
	if m.pg == nil {
		return
	}
	var enabled bool
	var message string
	err := m.pg.QueryRow(context.Background(),
		`SELECT enabled, message FROM maintenance_mode LIMIT 1`).Scan(&enabled, &message)
	if err != nil {
		return
	}
	m.mu.Lock()
	m.enabled = enabled
	m.message = message
	m.lastCheck = time.Now()
	m.mu.Unlock()
}

// IsEnabled reports whether maintenance mode is active. It refreshes the
// cached flag if it is older than 30s, so toggling the flag takes effect
// without a restart but without a DB hit per request.
func (m *MaintenanceMode) IsEnabled() bool {
	// Hold the read lock only long enough to check staleness; refresh takes
	// the write lock and we must not upgrade while holding the read lock.
	m.mu.RLock()
	stale := m.pg != nil && time.Since(m.lastCheck) > 30*time.Second
	m.mu.RUnlock()
	if stale {
		m.refresh()
	}
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.enabled
}

// Message returns the cached operator-supplied message for the 503 body.
func (m *MaintenanceMode) Message() string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.message
}

// Middleware returns an HTTP middleware that blocks requests during maintenance.
// Health endpoints are exempt so probes can still detect liveness while the
// rest of the API returns 503.
func (m *MaintenanceMode) Middleware() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			// Health checks must remain reachable so orchestrators don't kill
			// the pod while it is intentionally returning 503 to users.
			if r.URL.Path == "/api/v1/health" || r.URL.Path == "/health" {
				next.ServeHTTP(w, r)
				return
			}
			if m.IsEnabled() {
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(http.StatusServiceUnavailable)
				json.NewEncoder(w).Encode(map[string]string{
					"error":   "maintenance",
					"message": m.Message(),
				})
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// ToggleHandler enables/disables maintenance mode (admin only)
// @Summary      Toggle maintenance mode
// @Tags         Admin
// @Accept       json
// @Produce      json
// @Param        body  body  MaintenanceToggleRequest  true  "Maintenance settings"
// @Success      200  {object}  map[string]any
// @Security     BearerAuth
// @Router       /admin/maintenance [post]
func (m *MaintenanceMode) ToggleHandler(w http.ResponseWriter, r *http.Request) {
	var req MaintenanceToggleRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
		return
	}
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}

	if m.pg == nil {
		writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
		return
	}
	if _, err := m.pg.Exec(r.Context(),
		`UPDATE maintenance_mode SET enabled = $1, message = $2, updated_by = $3, updated_at = now()`,
		req.Enabled, req.Message, userID); err != nil {
		writeError(w, "internal_error", "failed to update maintenance mode", http.StatusInternalServerError)
		return
	}
	// Eagerly refresh the cache so the new state takes effect immediately
	// rather than waiting up to 30s for the next staleness check.
	m.refresh()
	LogFromRequest(r.Context(), m.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "admin.maintenance_toggle",
		ResourceType: "maintenance_mode",
		Details:      map[string]any{"enabled": req.Enabled, "message": req.Message},
	})
	writeJSON(w, http.StatusOK, map[string]any{"enabled": req.Enabled, "message": req.Message})
}
