// internal/ota_test.go — Tests for OTA check endpoint.

package internal_test

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/Anan5a/iot-platform/internal"
	"github.com/go-chi/chi/v5"
)

func TestOTACheck_NoUpdate(t *testing.T) {
	h := internal.NewOTAHandler(nil)
	r := chi.NewRouter()
	r.Get("/api/v1/ota/check/{key}", h.CheckOTA)

	req := httptest.NewRequest("GET", "/api/v1/ota/check/AABBCCDDEEFF?current_ver=2.0.0", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusInternalServerError {
		t.Errorf("status = %d, want 500 when DB is unavailable", rec.Code)
	}
}
