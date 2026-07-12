// internal/database.go — PostgreSQL and ClickHouse connection pools.
// Both are shared by cmd/api and cmd/ingest.

package internal

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/ClickHouse/clickhouse-go/v2"
	"github.com/jackc/pgx/v5/pgxpool"
)

// ConnectPG opens a connection pool to PostgreSQL. Call pool.Close() on shutdown.
func ConnectPG(ctx context.Context, dsn string) (*pgxpool.Pool, error) {
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		return nil, fmt.Errorf("parse pg config: %w", err)
	}
	cfg.MaxConns = 20
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("connect pg: %w", err)
	}
	if err := pool.Ping(ctx); err != nil {
		return nil, fmt.Errorf("ping pg: %w", err)
	}
	slog.Info("connected to PostgreSQL")
	return pool, nil
}

// ConnectCH opens a connection to ClickHouse. Call conn.Close() on shutdown.
func ConnectCH(ctx context.Context, dsn string) (clickhouse.Conn, error) {
	opts, err := clickhouse.ParseDSN(dsn)
	if err != nil {
		return nil, fmt.Errorf("parse ch dsn: %w", err)
	}
	conn, err := clickhouse.Open(opts)
	if err != nil {
		return nil, fmt.Errorf("connect ch: %w", err)
	}
	if err := conn.Ping(ctx); err != nil {
		return nil, fmt.Errorf("ping ch: %w", err)
	}
	slog.Info("connected to ClickHouse")
	return conn, nil
}
