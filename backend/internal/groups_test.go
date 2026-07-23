// internal/groups_test.go — Tests for device groups and tags handlers.

package internal_test

import (
	"bytes"
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/Anan5a/iot-platform/internal"
)

func TestCreateGroup_Validation(t *testing.T) {
	h := internal.NewGroupHandler(nil)
	body := `{"name": ""}`
	req := httptest.NewRequest("POST", "/api/v1/groups", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.CreateGroup(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}

func TestSetTag_RequiresOwnership(t *testing.T) {
	h := internal.NewGroupHandler(nil)

	req := httptest.NewRequest("POST", "/api/v1/devices/AABBCCDDEEFF/tags/location", bytes.NewBufferString(`{"value":"garage"}`))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.SetTag(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Errorf("status = %d, want 401 when no user context", rec.Code)
	}
}

func TestSetTag_UnownedDeviceRejected(t *testing.T) {
	h := internal.NewGroupHandler(nil)

	req := httptest.NewRequest("POST", "/api/v1/devices/AABBCCDDEEFF/tags/location", bytes.NewBufferString(`{"value":"garage"}`))
	req.Header.Set("Content-Type", "application/json")
	req = req.WithContext(context.WithValue(req.Context(), internal.ContextUserID, "user-1"))
	rec := httptest.NewRecorder()
	h.SetTag(rec, req)

	if rec.Code != http.StatusNotFound {
		t.Errorf("status = %d, want 404 when DB is nil (device not found)", rec.Code)
	}
}

func TestDeleteTag_RequiresOwnership(t *testing.T) {
	h := internal.NewGroupHandler(nil)

	req := httptest.NewRequest("DELETE", "/api/v1/devices/AABBCCDDEEFF/tags/location", nil)
	rec := httptest.NewRecorder()
	h.DeleteTag(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Errorf("status = %d, want 401 when no user context", rec.Code)
	}
}
