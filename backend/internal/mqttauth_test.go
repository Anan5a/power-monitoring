// internal/mqttauth_test.go — Tests for Mosquitto auth handler.

package internal

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestMQTTAuthHandler_MissingBody(t *testing.T) {
	h := &MQTTAuthHandler{}
	req := httptest.NewRequest("POST", "/api/v1/mqtt/auth", bytes.NewReader(nil))
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}
