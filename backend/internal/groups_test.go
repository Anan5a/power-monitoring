// internal/groups_test.go — Tests for device groups handler.

package internal

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestCreateGroup_Validation(t *testing.T) {
	h := &GroupHandler{}
	body := `{"name": ""}`
	req := httptest.NewRequest("POST", "/api/v1/groups", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.CreateGroup(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}
