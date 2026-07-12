// internal/search_test.go — Tests for search handler.

package internal

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestSearch_EmptyQuery(t *testing.T) {
	h := &SearchHandler{}
	req := httptest.NewRequest("GET", "/api/v1/search?q=", nil)
	rec := httptest.NewRecorder()
	h.Search(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}
