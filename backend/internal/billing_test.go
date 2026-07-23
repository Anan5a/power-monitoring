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

func TestCreateInvoice_InvalidDates(t *testing.T) {
	h := internal.NewBillingHandler(nil)
	body := `{
		"user_id": "user-1",
		"plan_id": 1,
		"audience": "user",
		"period_start": "not-a-date",
		"period_end": "2026-07-31",
		"amount_cents": 999,
		"description": "Pro plan"
	}`
	req := httptest.NewRequest("POST", "/api/v1/billing/invoices", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.CreateInvoice(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400 for invalid date", rec.Code)
	}
}

func TestCreateInvoice_PeriodEndBeforeStart(t *testing.T) {
	h := internal.NewBillingHandler(nil)
	body := `{
		"user_id": "user-1",
		"plan_id": 1,
		"audience": "user",
		"period_start": "2026-08-01",
		"period_end": "2026-07-01",
		"amount_cents": 999,
		"description": "Pro plan"
	}`
	req := httptest.NewRequest("POST", "/api/v1/billing/invoices", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.CreateInvoice(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400 for period_end before period_start", rec.Code)
	}
}
