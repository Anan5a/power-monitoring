# IoT Platform Backend

Self-hosted IoT platform backend for the power-monitoring ecosystem. Two Go binaries share one module and the `internal/` packages.

| Binary | Role |
|---|---|
| `api` (`cmd/api`) | HTTP/REST, WebSocket live push, Mosquitto auth backend, alert engine, email, OTA, billing, admin |
| `ingest` (`cmd/ingest`) | MQTT consumer: parse → validate → enrich → batch-write to ClickHouse → republish `live/#` |

## Quick start

Requires Docker and Docker Compose.

```bash
cp .env.example .env
# Edit .env: set JWT_SECRET to a random 64-character string.

make up          # start postgres, clickhouse, mosquitto, minio, api, ingest
make migrate-up  # run SQL migrations
make seed-dev    # create a test user + device
```

After `seed-dev` you will see credentials:

- User: `test@example.com` / `TestPass123!`
- Device key: `AABBCCDDEEFF` (already claimed)

Smoke test:

```bash
# Login
TOKEN=$(curl -s -X POST http://localhost:8080/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"test@example.com","password":"TestPass123!"}' | jq -r '.access_token')

# List devices
curl -s http://localhost:8080/api/v1/devices -H "Authorization: Bearer $TOKEN"

# Publish telemetry (device uses its API key, not the JWT)
mosquitto_pub -h localhost -u AABBCCDDEEFF -P '<api-key-from-seed-output>' \
  -t telemetry/power_monitor_v2/AABBCCDDEEFF \
  -m '{"ts":1720000000,"ts_ms":0,"schema":"telemetry_v1","fw":"2.0.0","uptime_ms":3600000,"rssi":-55,"heap_free":150000,"data":{"ch0_P":19.8}}'

# Query latest telemetry
curl -s http://localhost:8080/api/v1/telemetry/AABBCCDDEEFF/latest \
  -H "Authorization: Bearer $TOKEN"
```

## Development commands

```bash
make build          # build api + ingest binaries
make build-seed     # build seed utility
make test           # unit tests (no Docker)
make test-integration # testcontainers integration tests (Docker required)
make test-cover     # coverage report
make lint           # golangci-lint
make swag-init      # regenerate Swagger docs
make clean          # remove bin/ and cover.out
```

## Test environment notes

- The local Mosquitto container uses `allow_anonymous true` for easy testing; leave `MQTT_USER`/`MQTT_PASSWORD` empty in `.env`.
- Production Mosquitto HTTP auth requires a custom image or plugin with `/usr/lib/mosquitto/auth_plugin_http.so`.
- MinIO S3 API is on host port `9002`; console on `9003`.
- ClickHouse native protocol is on host port `9000`.
- `clickhouse/config/default-user.xml` allows external Docker connections with an empty password for the default user.

## Architecture

See `docs/superpowers/specs/2026-07-12-iot-platform-backend-design.md` for the full design. Ingest is pure data plumbing; all business logic lives in `api`.
