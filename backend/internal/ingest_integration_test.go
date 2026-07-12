// backend/internal/ingest_integration_test.go
//go:build integration

package internal

import (
	"context"
	"testing"

	"github.com/testcontainers/testcontainers-go"
	"github.com/testcontainers/testcontainers-go/wait"
)

func TestIntegration_PostgreSQL(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test")
	}

	ctx := context.Background()
	req := testcontainers.ContainerRequest{
		Image:        "postgres:16-alpine",
		ExposedPorts: []string{"5432/tcp"},
		Env: map[string]string{
			"POSTGRES_DB":       "test",
			"POSTGRES_USER":     "test",
			"POSTGRES_PASSWORD": "test",
		},
		WaitingFor: wait.ForLog("database system is ready to accept connections"),
	}
	pgC, err := testcontainers.GenericContainer(ctx, testcontainers.GenericContainerRequest{
		ContainerRequest: req,
		Started:          true,
	})
	if err != nil {
		t.Fatalf("start postgres: %v", err)
	}
	defer pgC.Terminate(ctx)

	host, _ := pgC.Host(ctx)
	port, _ := pgC.MappedPort(ctx, "5432")
	dsn := "postgres://test:test@" + host + ":" + port.Port() + "/test?sslmode=disable"

	pg, err := ConnectPG(ctx, dsn)
	if err != nil {
		t.Fatalf("ConnectPG: %v", err)
	}
	defer pg.Close()

	// Run migration
	_, err = pg.Exec(ctx, `
		CREATE TABLE IF NOT EXISTS users (
			id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
			email TEXT UNIQUE NOT NULL,
			password_hash TEXT NOT NULL,
			display_name TEXT,
			role TEXT NOT NULL DEFAULT 'user',
			created_at TIMESTAMPTZ DEFAULT now(),
			updated_at TIMESTAMPTZ DEFAULT now()
		)`)
	if err != nil {
		t.Fatalf("create table: %v", err)
	}

	// Insert a user
	_, err = pg.Exec(ctx, `INSERT INTO users (email, password_hash) VALUES ($1, $2)`,
		"test@example.com", "$2a$12$hash")
	if err != nil {
		t.Fatalf("insert user: %v", err)
	}

	// Query
	var count int
	pg.QueryRow(ctx, `SELECT count(*) FROM users`).Scan(&count)
	if count != 1 {
		t.Errorf("user count = %d, want 1", count)
	}
}
