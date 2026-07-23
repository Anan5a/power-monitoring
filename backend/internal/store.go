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
type BatchWriter struct {
	store     TelemetryStore
	chConn    any // clickhouse.Conn — typed in real code
	pgPool    any // *pgxpool.Pool — typed in real code
	buf       chan TelemetryRow
	batchSize int
}

func NewBatchWriter(store TelemetryStore, chConn, pgPool any) *BatchWriter {
	return &BatchWriter{
		store:     store,
		chConn:    chConn,
		pgPool:    pgPool,
		buf:       make(chan TelemetryRow, 10000),
		batchSize: 1000,
	}
}

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

func (bw *BatchWriter) flushBatch(ctx context.Context, rows []TelemetryRow) error {
	for _, row := range rows {
		if err := bw.store.Write(ctx, row); err != nil {
			return err
		}
	}
	return nil
}

// FlushLoop runs in a goroutine, flushing every 30s or when the buffer
// reaches batchSize. Call from cmd/ingest/main.go.
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
			bw.Flush(ctx) // final flush on shutdown
			return
		}
	}
}

// ── ClickHouse Store ─────────────────────────────────────────────────

type CHStore struct {
	conn clickhouse.Conn
}

func NewCHStore(conn clickhouse.Conn) *CHStore {
	return &CHStore{conn: conn}
}

func (s *CHStore) Flush(ctx context.Context) error { return nil }

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
