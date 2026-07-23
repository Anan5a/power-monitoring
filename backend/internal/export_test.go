// internal/export_test.go — Tests for data export handler.

package internal_test

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/Anan5a/iot-platform/internal"
)

func TestGetExportStatus_NotFound(t *testing.T) {
	h := internal.NewExportHandler(nil, nil, "firmware", "http://localhost:8080")
	req := httptest.NewRequest("GET", "/api/v1/export/status/nonexistent", nil)
	req = req.WithContext(context.WithValue(req.Context(), internal.ContextUserID, "user-1"))
	rec := httptest.NewRecorder()
	h.GetExportStatus(rec, req)
	if rec.Code != http.StatusInternalServerError {
		t.Errorf("status = %d, want 500 when DB is unavailable", rec.Code)
	}
}
