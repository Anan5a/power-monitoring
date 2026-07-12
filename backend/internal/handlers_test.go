// internal/handlers_test.go — Tests for REST API handlers.

package internal

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestRegister_Validation(t *testing.T) {
	h := &Handlers{}
	body := `{"email": "", "password": ""}`
	req := httptest.NewRequest("POST", "/api/v1/auth/register", bytes.NewBufferString(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.Register(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}

func TestHealth(t *testing.T) {
	// This test doesn't need a real DB — it just checks the handler exists
	h := &Handlers{}
	req := httptest.NewRequest("GET", "/api/v1/health", nil)
	rec := httptest.NewRecorder()
	h.Health(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status = %d, want 200", rec.Code)
	}
}
