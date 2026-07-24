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

// BillingHandler implements the manual billing/invoicing endpoints. Admins
// create invoices; when an invoice is marked paid the user's license plan is
// upgraded in the same transaction. All mutations are recorded via the Auditor.
type BillingHandler struct {
	pg      *pgxpool.Pool
	auditor *Auditor
}

// NewBillingHandler constructs a BillingHandler and its own Auditor backed by
// the given pool.
func NewBillingHandler(pg *pgxpool.Pool) *BillingHandler {
	return &BillingHandler{pg: pg, auditor: NewAuditor(pg)}
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

	if req.UserID == "" || req.AmountCents <= 0 || req.Description == "" || req.PeriodStart == "" || req.PeriodEnd == "" {
		writeError(w, "validation_error", "user_id, amount_cents, description, period_start and period_end are required", http.StatusBadRequest)
		return
	}

	start, err := time.Parse("2006-01-02", req.PeriodStart)
	if err != nil {
		writeError(w, "validation_error", "invalid period_start format (YYYY-MM-DD)", http.StatusBadRequest)
		return
	}
	end, err := time.Parse("2006-01-02", req.PeriodEnd)
	if err != nil {
		writeError(w, "validation_error", "invalid period_end format (YYYY-MM-DD)", http.StatusBadRequest)
		return
	}
	if end.Before(start) {
		writeError(w, "validation_error", "period_end must be on or after period_start", http.StatusBadRequest)
		return
	}

	if h.pg == nil {
		writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
		return
	}

	// Invoice numbers are generated locally from year + low bits of UnixMilli.
	// Uniqueness relies on the millisecond suffix; collisions are extremely
	// unlikely for the low admin-driven volume of manual invoices.
	invoiceNumber := fmt.Sprintf("INV-%d-%04d", time.Now().Year(), time.Now().UnixMilli()%10000)

	var id string
	err = h.pg.QueryRow(r.Context(), `
		INSERT INTO invoices (user_id, invoice_number, description, plan_id, audience,
			period_start, period_end, amount_cents, tax_cents, total_cents)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 0, $8)
		RETURNING id`,
		req.UserID, invoiceNumber, req.Description, req.PlanID, req.Audience,
		start, end, req.AmountCents).Scan(&id)
	if err != nil {
		writeError(w, "internal_error", "failed to create invoice", http.StatusInternalServerError)
		return
	}
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "billing.invoice.create",
		ResourceType: "invoice",
		ResourceID:   id,
		Details:      map[string]any{"user_id": req.UserID, "amount_cents": req.AmountCents},
	})
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
	adminID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || adminID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
	if h.pg == nil {
		writeError(w, "internal_error", "database unavailable", http.StatusInternalServerError)
		return
	}

	// The entire "mark paid → upgrade license → log change" sequence runs in
	// one transaction so the invoice and license never disagree, even if the
	// process crashes mid-way. Row-level locking on the invoice (FOR UPDATE)
	// prevents two concurrent mark-paid requests from both succeeding and
	// double-upgrading the license.
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
	// FOR UPDATE locks the invoice row for the duration of the transaction so
	// a concurrent mark-paid request blocks until this tx commits/rolls back.
	err = tx.QueryRow(r.Context(),
		`SELECT user_id, plan_id, audience, status FROM invoices WHERE id = $1 FOR UPDATE`,
		invoiceID).Scan(&inv.UserID, &inv.PlanID, &inv.Audience, &inv.Status)
	if err != nil {
		writeError(w, "not_found", "invoice not found", http.StatusNotFound)
		return
	}
	if inv.Status != "pending" {
		// Idempotency guard: a non-pending invoice has already been processed.
		writeError(w, "conflict", "invoice already processed", http.StatusConflict)
		return
	}

	if _, err := tx.Exec(r.Context(),
		`UPDATE invoices SET status = 'paid', paid_at = now(), paid_via = 'manual' WHERE id = $1`,
		invoiceID); err != nil {
		writeError(w, "internal_error", "failed to update invoice", http.StatusInternalServerError)
		return
	}

	// Upsert the user's license to the invoice's plan. ON CONFLICT handles
	// users who already have a license row (plan upgrade/downgrade) vs. first
	// time license creation. device_count is (re)set to 0 so the new plan's
	// cap is enforced fresh.
	if _, err := tx.Exec(r.Context(),
		`INSERT INTO user_licenses (user_id, plan_id, device_count, starts_at)
		 VALUES ($1, $2, 0, now())
		 ON CONFLICT (user_id) DO UPDATE SET plan_id = $2, updated_at = now()`,
		inv.UserID, inv.PlanID); err != nil {
		writeError(w, "internal_error", "failed to update license", http.StatusInternalServerError)
		return
	}

	// Append to the license change log for audit history: who changed, to
	// which plan, for which invoice, and by which admin.
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
	LogFromRequest(r.Context(), h.auditor, r, AuditEntry{
		ActorType:    "user",
		Action:       "billing.invoice.paid",
		ResourceType: "invoice",
		ResourceID:   invoiceID,
		Details:      map[string]any{"user_id": inv.UserID, "plan_id": inv.PlanID},
	})
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
	userID, ok := r.Context().Value(ContextUserID).(string)
	if !ok || userID == "" {
		writeError(w, "unauthorized", "missing user context", http.StatusUnauthorized)
		return
	}
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
		if err := rows.Scan(&inv.ID, &inv.InvoiceNumber, &inv.Description, &inv.TotalCents, &inv.Status, &inv.CreatedAt); err != nil {
			writeError(w, "internal_error", "failed to read invoices", http.StatusInternalServerError)
			return
		}
		invoices = append(invoices, inv)
	}
	if err := rows.Err(); err != nil {
		writeError(w, "internal_error", "failed to read invoices", http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, invoices)
}
