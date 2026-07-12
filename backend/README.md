# IoT Platform Backend

Self-hosted IoT platform replacing Supabase. Two binaries:

- **api** — HTTP/WebSocket server (auth, REST, device management, live data)
- **ingest** — MQTT consumer (telemetry pipeline, ClickHouse storage)

## Quick start

```bash
cp .env.example .env   # edit JWT_SECRET
make up                # docker compose up -d
make migrate-up        # run database migrations
```

## Development

```bash
make test              # unit tests
make test-integration  # integration tests (requires Docker)
make lint              # golangci-lint
```

## Architecture

See `docs/superpowers/specs/2026-07-12-iot-platform-backend-design.md`.
