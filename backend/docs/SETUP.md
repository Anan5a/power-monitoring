# IoT Platform Backend — Self-Hosted Setup

This guide replaces the old Supabase setup instructions. It covers running the new Go backend locally with Docker Compose.

## Prerequisites

- Docker + Docker Compose
- `go 1.22` or later (for local builds/tests)
- `golang-migrate/migrate` CLI (used by `make migrate-up`)

## 1. Configure environment

```bash
cp .env.example .env
```

Edit `.env`. The only value you must change is `JWT_SECRET`:

```env
JWT_SECRET=replace-with-a-random-64-character-string-minimum
```

All other defaults point at the Docker Compose service names (`postgres`, `clickhouse`, `mosquitto`, `minio`).

## 2. Start infrastructure

```bash
make up
```

This starts:

- `postgres` on `5432`
- `clickhouse` on `8123` (HTTP) and `9000` (native)
- `mosquitto` on `1883`
- `minio` S3 API on `9002`, console on `9003`
- `api` on `8080`
- `ingest` (no exposed port)

A one-shot `minio-init` service creates the `firmware` bucket.

> Note: the local Mosquitto container uses `allow_anonymous true` for easy local testing.
> Leave `MQTT_USER`/`MQTT_PASSWORD` empty in `.env` when using this test config.
> Production deployments should switch to the HTTP auth backend (see `mosquitto.conf`);
> when using password-file auth, set `MQTT_USER`/`MQTT_PASSWORD` and create matching entries in `mosquitto/config/passwd`.

## 3. Run migrations

```bash
make migrate-up
```

This applies `migrations/001_initial.up.sql`, `002_ota_alerts.up.sql`, and `003_billing_oauth.up.sql` to Postgres.

## 4. Seed development data

```bash
make seed-dev
```

Output includes a test user and a pre-claimed device:

```
Seed complete
  User:    test@example.com
  Pass:    TestPass123!
  Device:  AABBCCDDEEFF
  API key: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

Save the API key — devices authenticate to Mosquitto with `device_key` as username and `api_key` as password.
(With the test Mosquitto config this is not strictly required because anonymous connections are allowed, but it mirrors production.)

## 5. Smoke test

### 5.1 Login

```bash
TOKEN=$(curl -s -X POST http://localhost:8080/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"test@example.com","password":"TestPass123!"}' | jq -r '.access_token')

echo $TOKEN
```

### 5.2 List devices

```bash
curl -s http://localhost:8080/api/v1/devices \
  -H "Authorization: Bearer $TOKEN"
```

### 5.3 Publish telemetry

Use `mosquitto_pub` or any MQTT client. The device authenticates with `device_key` / `api_key`.

```bash
mosquitto_pub -h localhost -u AABBCCDDEEFF -P 'xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx' \
  -t telemetry/power_monitor_v2/AABBCCDDEEFF \
  -m '{
    "ts":1720000000,
    "ts_ms":0,
    "schema":"telemetry_v1",
    "fw":"2.0.0",
    "uptime_ms":3600000,
    "rssi":-55,
    "heap_free":150000,
    "data":{"ch0_P":19.8,"ch1_P":-6.4,"energy_wh0":120.5,"soc_pct0":85.0}
  }'
```

### 5.5 Query latest telemetry

```bash
curl -s http://localhost:8080/api/v1/telemetry/AABBCCDDEEFF/latest \
  -H "Authorization: Bearer $TOKEN"
```

Expected response:

```json
{
  "device_key": "AABBCCDDEEFF",
  "recorded_at": "2026-07-23T...",
  "pv_power": 19.8,
  ...
}
```

### 5.6 WebSocket live stream

Connect to `ws://localhost:8080/api/v1/ws` with the JWT in an `Authorization: Bearer <token>` header (your WebSocket client must support headers), then send:

```json
{"type":"subscribe","device_keys":["AABBCCDDEEFF"]}
```

Each new telemetry message will be pushed as a JSON text message.

## 6. Useful commands

```bash
make down           # stop all containers
make migrate-down   # rollback one migration
make test           # unit tests
make test-integration # integration tests (requires Docker)
make lint           # lint
```

## 7. Troubleshooting

### Mosquitto auth failures

The dev stack uses `mosquitto/config/mosquitto-test.conf` (`allow_anonymous
true`), so no credentials are required. If you swap to `mosquitto.conf`
(production HTTP-auth backend) without the auth plugin compiled into the
image, Mosquitto will fail to start or reject all clients — see
`mosquitto/README.md` for building the plugin image. Leave
`MQTT_USER`/`MQTT_PASSWORD` empty in `.env` for the dev stack.

### MinIO bucket missing

`make up` should create it automatically via `minio-init`. If you need to recreate:

```bash
docker run --rm --network=host minio/mc \
  mc alias set local http://localhost:9002 minioadmin minioadmin
  mc mb local/firmware --ignore-existing
```

### API exits with "missing required config"

You forgot to set `JWT_SECRET` in `.env` or the `.env` file was not loaded. The Docker Compose services read `.env` from the `backend/` directory.

## 8. Next steps

- See `docs/API.md` for the full endpoint reference.
- See `docs/superpowers/specs/2026-07-12-iot-platform-backend-design.md` for architecture details.

## 9. Production deployment

A production overlay ships at `docker-compose.prod.yml`. It layers on top of
the base compose to put the API behind Traefik with automatic Let's Encrypt
HTTPS and add scheduled backup sidecars.

```bash
cp .env.example .env   # fill in real secrets (POSTGRES_PASSWORD, JWT_SECRET, ...)
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d
```

Required `.env` additions for prod:

```env
DOMAIN=iot.example.com
TRAEFIK_ACME_EMAIL=ops@example.com
POSTGRES_PASSWORD=<strong>
CLICKHOUSE_PASSWORD=<strong>
MINIO_ROOT_USER=<strong>
MINIO_ROOT_PASSWORD=<strong>
JWT_SECRET=<strong>
MQTT_USER=<strong>          # if running the HTTP-auth broker
MQTT_PASSWORD=<strong>
```

What the overlay does:

- **Traefik** terminates TLS (TLS-ALPN-01 challenge), redirects HTTP→HTTPS, and
  routes `${DOMAIN}` → `api:8080` (WebSocket-aware) and `minio.${DOMAIN}` →
  MinIO console. No service ports are published directly except `1883`.
- **Mosquitto** uses the HTTP-auth config (`mosquitto/config/mosquitto.conf`)
  via a custom plugin image — build it first (`mosquitto/README.md`). Never run
  `allow_anonymous true` in prod. Restrict `1883` to device subnets at the
  firewall.
- **Backups** run as sidecars on a daily schedule:
  - `postgres-backup` → `pgbackups` volume (daily, keeps 7d/4w/6m).
  - `clickhouse-backup` → `chbackups` volume (`BACKUP DATABASE` to zip, 7d).
  - `minio-backup` → `minio_backups` volume (`mc mirror` of the firmware
    bucket, 7d).
  Copy these volumes off-host (e.g. to S3) for disaster recovery.

For an internet-exposed broker, add a TLS listener on `8883` with client
certificates in `mosquitto.conf` instead of exposing `1883` directly.
