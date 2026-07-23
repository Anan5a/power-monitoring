// internal/billing_test.go — Tests for billing handler.

package internal_test

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/Anan5a/iot-platform/internal"
)

func TestCreateInvoice_Validation(t *testing.T) {
	h := internal.NewBillingHandler(nil)
	body := `{"user_id": ""}`
	req := httptest.NewRequest("POST", "/api/v1/billing/invoices", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.CreateInvoice(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}
