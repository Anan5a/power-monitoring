// internal/export_test.go — Tests for data export handler.

package internal

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestGetExportStatus_NotFound(t *testing.T) {
	h := &ExportHandler{}
	req := httptest.NewRequest("GET", "/api/v1/export/status/nonexistent", nil)
	rec := httptest.NewRecorder()
	h.GetExportStatus(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("status = %d, want 404", rec.Code)
	}
}
