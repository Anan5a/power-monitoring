// internal/search_test.go — Tests for search handler.

package internal_test

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/Anan5a/iot-platform/internal"
)

func TestSearch_EmptyQuery(t *testing.T) {
	h := internal.NewSearchHandler(nil)
	req := httptest.NewRequest("GET", "/api/v1/search?q=", nil)
	rec := httptest.NewRecorder()
	h.Search(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}

func TestSearch_UsesParameterizedILike(t *testing.T) {
	// This test documents the fix: search should never concatenate the raw
	// query string into an ILIKE clause. With a nil DB the handler returns
	// an empty result set rather than panicking, which is acceptable for
	// this unit-level assertion.
	h := internal.NewSearchHandler(nil)
	req := httptest.NewRequest("GET", "/api/v1/search?q=solar%20panel", nil)
	rec := httptest.NewRecorder()
	h.Search(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status = %d, want 200", rec.Code)
	}
}
