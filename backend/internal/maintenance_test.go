// internal/maintenance_test.go — Tests for maintenance mode.

package internal

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestMaintenanceMiddleware_AllowsHealth(t *testing.T) {
	m := &MaintenanceMode{enabled: true, message: "test"}
	handler := m.Middleware()(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest("GET", "/api/v1/health", nil)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("health check: status = %d, want 200", rec.Code)
	}
}

func TestMaintenanceMiddleware_BlocksOtherRoutes(t *testing.T) {
	m := &MaintenanceMode{enabled: true, message: "down for maintenance"}
	handler := m.Middleware()(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest("GET", "/api/v1/devices", nil)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Errorf("blocked route: status = %d, want 503", rec.Code)
	}
}
