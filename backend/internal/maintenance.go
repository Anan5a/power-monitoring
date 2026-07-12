// internal/maintenance.go — Maintenance mode toggle and middleware.
// When enabled, all endpoints (except /health) return 503.
// The ingest worker also checks the flag and pauses processing.

package internal

import (
	"encoding/json"
	"net/http"
	"sync"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

type MaintenanceMode struct {
	pg       *pgxpool.Pool
	mu       sync.RWMutex
	enabled  bool
	message  string
	lastCheck time.Time
}

func NewMaintenanceMode(pg *pgxpool.Pool) *MaintenanceMode {
	m := &MaintenanceMode{pg: pg}
	m.refresh()
	return m
}

func (m *MaintenanceMode) refresh() {
	var enabled bool
	var message string
	err := m.pg.QueryRow(nil,
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

func (m *MaintenanceMode) IsEnabled() bool {
	if time.Since(m.lastCheck) > 30*time.Second {
		m.refresh()
	}
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.enabled
}

func (m *MaintenanceMode) Message() string {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.message
}

// Middleware returns an HTTP middleware that blocks requests during maintenance.
func (m *MaintenanceMode) Middleware() func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
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

// ToggleHandler enables/disables maintenance mode. Admin only.
func (m *MaintenanceMode) ToggleHandler(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Enabled bool   `json:"enabled"`
		Message string `json:"message"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
		return
	}
	userID := r.Context().Value(ContextUserID).(string)
	m.pg.Exec(nil,
		`UPDATE maintenance_mode SET enabled = $1, message = $2, updated_by = $3, updated_at = now()`,
		req.Enabled, req.Message, userID)
	m.refresh()
	writeJSON(w, http.StatusOK, map[string]any{"enabled": req.Enabled, "message": req.Message})
}
