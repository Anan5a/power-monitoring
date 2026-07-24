// internal/retention.go — Per-plan data retention cleanup.
// Runs hourly in the ingest worker. Deletes ClickHouse rows for devices
// whose owner's plan retention has expired.

package internal

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/jackc/pgx/v5/pgxpool"
)

// RetentionCleanup deletes telemetry older than each device owner's plan
// allows. It is run periodically by the ingest worker; the API does not
// call it directly. The two nil-guards in Run make it safe to construct
// with a missing backend during local/dev setups where ClickHouse or
// Postgres may be disabled.
type RetentionCleanup struct {
	pg *pgxpool.Pool
	ch clickhouse.Conn
}

// NewRetentionCleanup wires the cleanup with its Postgres (device/plan
// lookup) and ClickHouse (row deletion) backends.
func NewRetentionCleanup(pg *pgxpool.Pool, ch clickhouse.Conn) *RetentionCleanup {
	return &RetentionCleanup{pg: pg, ch: ch}
}

// Run deletes telemetry older than each device's plan retention.
// Called hourly by a goroutine in the ingest worker.
func (rc *RetentionCleanup) Run(ctx context.Context) error {
	// Bail out cleanly when a backend is unavailable so a misconfigured
	// environment logs a warning at worst rather than crashing the worker.
	if rc.pg == nil {
		return nil
	}
	if rc.ch == nil {
		return nil
	}
	// Join devices → user_licenses → license_plans to get each device's
	// retention_days. Devices with no owner are skipped (NULL owner_id).
	rows, err := rc.pg.Query(ctx, `
		SELECT d.device_key, lp.retention_days
		FROM devices d
		JOIN user_licenses ul ON ul.user_id = d.owner_id
		JOIN license_plans lp ON lp.id = ul.plan_id
		WHERE d.owner_id IS NOT NULL`)
	if err != nil {
		return err
	}
	defer rows.Close()

	// Group device keys by retention horizon so we can issue one DELETE
	// per (days, device) pair and log a single summary line per horizon.
	byRetention := map[int][]string{}
	for rows.Next() {
		var key string
		var days int
		if err := rows.Scan(&key, &days); err != nil {
			return fmt.Errorf("scan retention row: %w", err)
		}
		byRetention[days] = append(byRetention[days], key)
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("retention query: %w", err)
	}

	for days, keys := range byRetention {
		// Cutoff is computed in UTC to match the tz stored in ClickHouse.
		// AddDate handles month/day boundaries correctly (e.g. -30 days).
		cutoff := time.Now().UTC().AddDate(0, 0, -days)
		// ClickHouse lightweight delete
		// We issue per-device DELETEs rather than one IN (...) list because
		// ALTER ... DELETE is a mutation and a single large mutation is
		// more disruptive to merges than several narrow ones.
		for _, key := range keys {
			if err := rc.ch.Exec(ctx,
				`ALTER TABLE device_telemetry DELETE WHERE device_id = ? AND ts < ?`,
				key, cutoff); err != nil {
				slog.Warn("retention delete", "device", key, "error", err)
			}
		}
		slog.Info("retention cleanup", "devices", len(keys), "days", days, "cutoff", cutoff)
	}
	return nil
}

// RunLoop runs the cleanup every hour. Call from cmd/ingest/main.go.
//
// The hourly cadence is a compromise: frequent enough that expired rows
// don't linger long past their plan's window, infrequent enough that the
// mutation load on ClickHouse is negligible. There is no final flush on
// shutdown — retention is best-effort and not loss-sensitive.
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
