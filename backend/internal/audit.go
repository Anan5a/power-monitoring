// internal/audit.go — Audit log writer. Logs user, device, and system
// actions to PostgreSQL for compliance and debugging.

package internal

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// Auditor writes AuditEntry rows to the audit_log table. It is used for
// compliance and post-incident debugging of user/device/system actions.
type Auditor struct {
	pg *pgxpool.Pool
}

// NewAuditor constructs an Auditor backed by the given pool.
func NewAuditor(pg *pgxpool.Pool) *Auditor {
	return &Auditor{pg: pg}
}

// Log persists one audit entry. Empty actor/IP/user-agent values are stored
// as SQL NULL (via nullIfEmpty) rather than empty strings so nullable columns
// stay nullable. Details is JSON-encoded; a marshal error is ignored (logged
// as "null" effectively) because auditing must never block the action.
func (a *Auditor) Log(ctx context.Context, entry AuditEntry) error {
	details, _ := json.Marshal(entry.Details)
	_, err := a.pg.Exec(ctx, `
		INSERT INTO audit_log (actor_id, actor_type, action, resource_type, resource_id, details, ip_address, user_agent, created_at)
		VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)`,
		nullIfEmpty(entry.ActorID),
		entry.ActorType,
		entry.Action,
		entry.ResourceType,
		entry.ResourceID,
		string(details),
		nullIfEmpty(entry.IPAddress),
		nullIfEmpty(entry.UserAgent),
		time.Now(),
	)
	if err != nil {
		return fmt.Errorf("audit log: %w", err)
	}
	return nil
}

// nullIfEmpty maps an empty string to a nil pointer so pgx inserts SQL NULL
// into the nullable column instead of an empty string. This keeps DISTINCT
// queries and "IS NULL" filters meaningful.
func nullIfEmpty(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}

// LogFromRequest fills actor_id, IP, and user-agent from an HTTP request and writes the audit entry.
// It is a best-effort side effect: errors from a.Log are dropped (the request
// path must not fail because audit logging failed).
func LogFromRequest(ctx context.Context, a *Auditor, r *http.Request, entry AuditEntry) {
	if a == nil {
		return
	}
	if userID, ok := r.Context().Value(ContextUserID).(string); ok && userID != "" {
		entry.ActorID = userID
	}
	entry.IPAddress = r.RemoteAddr
	entry.UserAgent = r.UserAgent()
	_ = a.Log(ctx, entry)
}
