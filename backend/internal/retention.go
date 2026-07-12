// internal/retention.go — Per-plan data retention cleanup.
// Runs hourly in the ingest worker. Deletes ClickHouse rows for devices
// whose owner's plan retention has expired.

package internal

import (
	"context"
	"log/slog"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/jackc/pgx/v5/pgxpool"
)

type RetentionCleanup struct {
	pg *pgxpool.Pool
	ch clickhouse.Conn
}

func NewRetentionCleanup(pg *pgxpool.Pool, ch clickhouse.Conn) *RetentionCleanup {
	return &RetentionCleanup{pg: pg, ch: ch}
}

// Run deletes telemetry older than each device's plan retention.
// Called hourly by a goroutine in the ingest worker.
func (rc *RetentionCleanup) Run(ctx context.Context) error {
	rows, err := rc.pg.Query(ctx, `
		SELECT d.device_key, lp.retention_days
		FROM devices d
		JOIN user_licenses ul ON ul.user_id = d.owner_id
		JOIN license_plans lp ON lp.id = ul.plan_id
		WHERE d.owner_id IS NOT NULL AND d.is_active = true`)
	if err != nil {
		return err
	}
	defer rows.Close()

	byRetention := map[int][]string{}
	for rows.Next() {
		var key string
		var days int
		rows.Scan(&key, &days)
		byRetention[days] = append(byRetention[days], key)
	}

	for days, keys := range byRetention {
		cutoff := time.Now().AddDate(0, 0, -days)
		// ClickHouse lightweight delete
		for _, key := range keys {
			rc.ch.Exec(ctx,
				`ALTER TABLE device_telemetry DELETE WHERE device_id = $1 AND ts < $2`,
				key, cutoff)
		}
		slog.Info("retention cleanup", "devices", len(keys), "days", days, "cutoff", cutoff)
	}
	return nil
}

// RunLoop runs the cleanup every hour. Call from cmd/ingest/main.go.
func (rc *RetentionCleanup) RunLoop(ctx context.Context) {
	ticker := time.NewTicker(1 * time.Hour)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			if err := rc.Run(ctx); err != nil {
				slog.Error("retention cleanup", "error", err)
			}
		case <-ctx.Done():
			return
		}
	}
}
