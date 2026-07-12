// internal/audit.go — Audit log writer. Logs user, device, and system
// actions to PostgreSQL for compliance and debugging.

package internal

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

type Auditor struct {
	pg *pgxpool.Pool
}

func NewAuditor(pg *pgxpool.Pool) *Auditor {
	return &Auditor{pg: pg}
}

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

func nullIfEmpty(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}
