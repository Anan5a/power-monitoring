// internal/store.go — ClickHouse batch writer with in-memory buffer.
// The ingest worker writes TelemetryRows here; FlushLoop periodically
// sends them to ClickHouse in batches.

package internal

import (
	"context"
	"log/slog"
	"time"

	"github.com/ClickHouse/clickhouse-go/v2"
)

// TelemetryStore is the interface for storing telemetry. The real
// implementation writes to ClickHouse; the fake stores in memory.
type TelemetryStore interface {
	Write(ctx context.Context, row TelemetryRow) error
	Flush(ctx context.Context) error
}

// BatchWriter buffers TelemetryRows and flushes them to ClickHouse
// in batches. Safe for concurrent use from multiple goroutines.
//
// Buffering trades a small amount of memory for decoupling: the ingest
// pipeline never blocks on ClickHouse latency, and a transient CH
// outage is absorbed up to the buffer's capacity before rows are dropped.
type BatchWriter struct {
	store     TelemetryStore
	buf       chan TelemetryRow
	batchSize int
}

// NewBatchWriter constructs a BatchWriter with a 10k-row buffered channel
// and a 1k-row flush batch. The buffer size is chosen to absorb a few
// minutes of peak ingest traffic; beyond that rows are dropped (see Write).
func NewBatchWriter(store TelemetryStore) *BatchWriter {
	return &BatchWriter{
		store:     store,
		buf:       make(chan TelemetryRow, 10000),
		batchSize: 1000,
	}
}

// Write appends a row to the buffer. It never blocks: if the buffer is
// full the row is dropped and logged rather than stalling the MQTT
// consumer (a stuck consumer would backpressure the broker and risk
// dropping the whole subscription). The error is always nil because the
// caller (the pipeline) cannot meaningfully react to a drop — the row
// is gone regardless.
func (bw *BatchWriter) Write(ctx context.Context, row TelemetryRow) error {
	select {
	case bw.buf <- row:
		return nil
	default:
		slog.Warn("ingest buffer full, dropping row", "device", row.DeviceID)
		return nil
	}
}

// Flush sends all buffered rows to the underlying store.
// Loops until the buffer is empty, flushing in batches of batchSize.
func (bw *BatchWriter) Flush(ctx context.Context) error {
	for {
		batch := make([]TelemetryRow, 0, bw.batchSize)
		// Drain up to batchSize rows
		for i := 0; i < bw.batchSize; i++ {
			select {
			case row := <-bw.buf:
				batch = append(batch, row)
			default:
				// Buffer empty
				if len(batch) > 0 {
					return bw.flushBatch(ctx, batch)
				}
				return nil
			}
		}
		// Batch full, flush and continue
		if err := bw.flushBatch(ctx, batch); err != nil {
			return err
		}
	}
}

// flushBatch writes a slice of rows to the underlying store. It writes
// each row, continuing past individual failures so one bad row does
// not silently drop the rest of the batch. The first error is returned so
// the caller can log it; failed rows are still consumed (removed from the
// buffer) to avoid an unbounded retry loop on a persistently bad row.
func (bw *BatchWriter) flushBatch(ctx context.Context, rows []TelemetryRow) error {
	var firstErr error
	for _, row := range rows {
		if err := bw.store.Write(ctx, row); err != nil && firstErr == nil {
			firstErr = err
			slog.Error("telemetry write failed", "device", row.DeviceID, "error", err)
		}
	}
	return firstErr
}

// FlushLoop runs in a goroutine, flushing every 30s or when the buffer
// reaches batchSize. Call from cmd/ingest/main.go.
//
// The 30s ticker bounds the maximum latency a row sits in the buffer
// before reaching ClickHouse in the steady state; on shutdown a final
// flush runs with a fresh 10s context so a cancelled parent context does
// not discard the last batch.
func (bw *BatchWriter) FlushLoop(ctx context.Context) {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			if err := bw.Flush(ctx); err != nil {
				slog.Error("batch flush failed", "error", err)
			}
		case <-ctx.Done():
			// Final flush with a fresh context so a cancelled parent context
			// does not cause the last batch to be dropped on shutdown.
			shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			if err := bw.Flush(shutdownCtx); err != nil {
				slog.Error("shutdown flush failed", "error", err)
			}
			return
		}
	}
}

// ── ClickHouse Store ─────────────────────────────────────────────────

// CHStore is the production TelemetryStore backed by ClickHouse. Each
// Write executes a single INSERT; batching is the caller's responsibility
// (see BatchWriter). Flush is a no-op because there is no client-side
// buffer to drain.
type CHStore struct {
	conn clickhouse.Conn
}

// NewCHStore returns a CHStore wrapping the given ClickHouse connection.
// The connection is expected to be pooled by the caller.
func NewCHStore(conn clickhouse.Conn) *CHStore {
	return &CHStore{conn: conn}
}

// Flush is a no-op: CHStore writes through immediately and keeps no
// client-side buffer. It exists only to satisfy the TelemetryStore
// interface so CHStore can be used directly (without a BatchWriter).
func (s *CHStore) Flush(ctx context.Context) error { return nil }

// Write inserts one telemetry row into the device_telemetry table.
// The `?` placeholders are clickhouse-go's native positional binding
// (not pgx-style `$1`); values are substituted in order. `ingested_at`
// is set server-side via now() so rows carry ClickHouse's wall time
// rather than the client's, which may drift.
func (s *CHStore) Write(ctx context.Context, row TelemetryRow) error {
	return s.conn.Exec(ctx, `
		INSERT INTO device_telemetry (
			device_id, device_type, ts, rssi, uptime_ms,
			pv_power, battery_power, inverter_power, dc_load_power, system_status,
			min_soc_pct, max_soc_pct, total_energy_wh, fields, ingested_at
		) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, now())`,
		row.DeviceID, row.DeviceType, row.Timestamp, row.RSSI, row.UptimeMS,
		row.PVPower, row.BatteryPower, row.InverterPower, row.DCLoadPower, row.SystemStatus,
		row.MinSOCPct, row.MaxSOCPct, row.TotalEnergyWh, row.Fields,
	)
}
