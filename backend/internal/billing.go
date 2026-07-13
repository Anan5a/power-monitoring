// internal/billing.go — Manual billing/invoicing. Admin creates invoices,
// marks them paid, and the system auto-upgrades the user's license plan.

package internal

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type BillingHandler struct {
	pg *pgxpool.Pool
}

func NewBillingHandler(pg *pgxpool.Pool) *BillingHandler {
	return &BillingHandler{pg: pg}
}

// CreateInvoice creates a new invoice (admin only)
// @Summary      Create invoice
// @Tags         Billing
// @Accept       json
// @Produce      json
// @Param        body  body  CreateInvoiceRequest  true  "Invoice details"
// @Success      201  {object}  map[string]string
// @Failure      400  {object}  APIError
// @Security     BearerAuth
// @Router       /billing/invoices [post]
func (h *BillingHandler) CreateInvoice(w http.ResponseWriter, r *http.Request) {
	var req CreateInvoiceRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
		return
	}

	start, _ := time.Parse("2006-01-02", req.PeriodStart)
	end, _ := time.Parse("2006-01-02", req.PeriodEnd)
	invoiceNumber := fmt.Sprintf("INV-%d-%04d", time.Now().Year(), time.Now().UnixMilli()%10000)

	var id string
	h.pg.QueryRow(r.Context(), `
		INSERT INTO invoices (user_id, invoice_number, description, plan_id, audience,
			period_start, period_end, amount_cents, tax_cents, total_cents)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 0, $8)
		RETURNING id`,
		req.UserID, invoiceNumber, req.Description, req.PlanID, req.Audience,
		start, end, req.AmountCents).Scan(&id)
	writeJSON(w, http.StatusCreated, map[string]string{"id": id, "invoice_number": invoiceNumber})
}

// MarkInvoicePaid marks an invoice as paid and upgrades the user's license (admin only)
// @Summary      Mark invoice as paid
// @Tags         Billing
// @Produce      json
// @Param        id  path  string  true  "Invoice ID"
// @Success      200  {object}  map[string]string
// @Failure      404  {object}  APIError
// @Failure      409  {object}  APIError
// @Security     BearerAuth
// @Router       /billing/invoices/{id}/mark-paid [post]
func (h *BillingHandler) MarkInvoicePaid(w http.ResponseWriter, r *http.Request) {
	invoiceID := chi.URLParam(r, "id")
	adminID := r.Context().Value(ContextUserID).(string)

	tx, err := h.pg.Begin(r.Context())
	if err != nil {
		writeError(w, "internal_error", "transaction failed", http.StatusInternalServerError)
		return
	}
	defer tx.Rollback(r.Context())

	var inv struct {
		UserID   string
		PlanID   int
		Audience string
		Status   string
	}
	err = tx.QueryRow(r.Context(),
		`SELECT user_id, plan_id, audience, status FROM invoices WHERE id = $1 FOR UPDATE`,
		invoiceID).Scan(&inv.UserID, &inv.PlanID, &inv.Audience, &inv.Status)
	if err != nil {
		writeError(w, "not_found", "invoice not found", http.StatusNotFound)
		return
	}
	if inv.Status != "pending" {
		writeError(w, "conflict", "invoice already processed", http.StatusConflict)
		return
	}

	if _, err := tx.Exec(r.Context(),
		`UPDATE invoices SET status = 'paid', paid_at = now(), paid_via = 'manual' WHERE id = $1`,
		invoiceID); err != nil {
		writeError(w, "internal_error", "failed to update invoice", http.StatusInternalServerError)
		return
	}

	if _, err := tx.Exec(r.Context(),
		`INSERT INTO user_licenses (user_id, plan_id, device_count, starts_at)
		 VALUES ($1, $2, 0, now())
		 ON CONFLICT (user_id) DO UPDATE SET plan_id = $2, updated_at = now()`,
		inv.UserID, inv.PlanID); err != nil {
		writeError(w, "internal_error", "failed to update license", http.StatusInternalServerError)
		return
	}

	if _, err := tx.Exec(r.Context(),
		`INSERT INTO license_change_log (user_id, audience, to_plan_id, reason, invoice_id, changed_by)
		 VALUES ($1, $2, $3, 'payment_received', $4, $5)`,
		inv.UserID, inv.Audience, inv.PlanID, invoiceID, adminID); err != nil {
		writeError(w, "internal_error", "failed to log license change", http.StatusInternalServerError)
		return
	}

	if err := tx.Commit(r.Context()); err != nil {
		writeError(w, "internal_error", "commit failed", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "paid"})
}

// ListInvoices returns invoices for the authenticated user
// @Summary      List invoices
// @Tags         Billing
// @Produce      json
// @Success      200  {array}  Invoice
// @Security     BearerAuth
// @Router       /billing/invoices [get]
func (h *BillingHandler) ListInvoices(w http.ResponseWriter, r *http.Request) {
	userID := r.Context().Value(ContextUserID).(string)
	rows, err := h.pg.Query(r.Context(), `
		SELECT id, invoice_number, description, total_cents, status, created_at
		FROM invoices WHERE user_id = $1 ORDER BY created_at DESC LIMIT 50`, userID)
	if err != nil {
		writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
		return
	}
	defer rows.Close()

	invoices := []Invoice{}
	for rows.Next() {
		var inv Invoice
		rows.Scan(&inv.ID, &inv.InvoiceNumber, &inv.Description, &inv.TotalCents, &inv.Status, &inv.CreatedAt)
		invoices = append(invoices, inv)
	}
	writeJSON(w, http.StatusOK, invoices)
}
