# Backend Architecture

This document describes the self-hosted Go backend for the ESP32 power-monitoring
platform: its binaries, components, data flows, data model, security model, and
deployment. It is the authoritative architectural reference; per-endpoint
details live in [`API.md`](API.md), and operational setup in [`SETUP.md`](SETUP.md).

## 1. Goals & scope

- **Ingest** high-frequency telemetry from ESP32 devices via MQTT into a
  columnar store (ClickHouse) for cheap long-term storage.
- **Serve** a REST + WebSocket API for the web UI: auth, device management,
  live telemetry, alerting, OTA, billing/licensing, GDPR export.
- **Command** devices asynchronously (relay overrides, config) through a polled
  queue.
- **Be self-hostable**: a single `docker compose` brings up the whole stack;
  a production overlay adds TLS termination and backups.
- **Multi-tenant by ownership**: every device belongs to a user; data access is
  scoped to the owner (and admins).

The backend deliberately keeps business logic out of the ingest hot path. The
ingest worker only parses, enriches, stores, and republishes; alerting, email,
and live push run in the API process.

## 2. High-level topology

```
                         ┌───────────────┐
   ESP32 devices ──MQTT──▶   Mosquitto   ─── telemetry/{type}/{key}
                         │   (broker)    ─── status/{key}/online
                         └──────┬────────┘
                ┌───────────────┼───────────────┐
                ▼               ▼               ▼
        ┌─────────────┐  ┌─────────────┐  (live/{key} republished
        │  ingest     │  │    api      │   by ingest; api subscribes)
        │ (MQTT→CH)   │  │ (HTTP/WS)   │
        └──────┬──────┘  └──────┬──────┘
               │                │
               ▼                ├──────────▶ WebSocket clients (browser)
        ┌─────────────┐         ├──────────▶ Alert engine → email queue
        │  ClickHouse │         ├──────────▶ Postgres (operational state)
        │  telemetry  │         └──────────▶ MinIO (firmware, exports)
        └─────────────┘
               ┌─────────────┐
               │  Postgres   │  ◀── shared by api + ingest + seed
               │ operational │
               └─────────────┘
               ┌─────────────┐
               │   MinIO/S3  │
               └─────────────┘
```

Two long-running binaries (`api`, `ingest`) and one utility (`seed`) share the
`internal` package for all domain logic.

## 3. Binaries

| Binary | Path | Role |
|---|---|---|
| `api` | `cmd/api/main.go` | HTTP REST + WebSocket server. Subscribes to `live/#` (for WS push + alert evaluation) and `status/+/online` (online/offline). |
| `ingest` | `cmd/ingest/main.go` | MQTT telemetry consumer. Parses, resolves, enriches, writes to ClickHouse, and republishes enriched payloads to `live/{key}`. Runs retention cleanup hourly. |
| `seed` | `cmd/seed/main.go` | Dev-only: creates a test user, an unclaimed device, and license plans so the smoke-test flow works out of the box. |

`api` and `ingest` both connect to Postgres, ClickHouse, and Mosquitto. They do
not talk to each other directly; they coordinate through MQTT topics and shared
state in Postgres.

## 4. Technology stack

- **Language:** Go 1.22, module `github.com/Anan5a/iot-platform`.
- **HTTP router:** `go-chi/chi/v5` with its `middleware` subpackage (RequestID,
  RealIP, Recoverer).
- **Postgres:** `jackc/pgx/v5` (native protocol, `$n` placeholders). Operational
  state and the audit log.
- **ClickHouse:** `ClickHouse/clickhouse-go/v2` (native protocol, **`?`
  placeholders** — not `$n`). Time-series telemetry.
- **MQTT:** `eclipse/paho.mqtt.golang`. Both binaries are clients.
- **WebSocket:** `gorilla/websocket` for live telemetry/status push.
- **Object storage:** `minio/minio-go/v7` for firmware binaries and GDPR exports.
- **Auth:** `golang-jwt/jwt/v5` (HS256), `golang.org/x/crypto/bcrypt`,
  `golang.org/x/oauth2` (Google, GitHub).
- **Email:** `gomail.v2` via an async DB-backed queue.
- **Docs:** `swaggo/swag` annotations → OpenAPI in `docs/api/`, served at
  `/api/v1/swagger/`.
- **Process:** `go.uber.org/automaxprocs` so GOMAXPROCS matches container CPU.

## 5. Package layout (`internal/`)

| File | Responsibility |
|---|---|
| `config.go` | Env loading + startup validation (incl. weak-secret rejection, CORS sanitising). |
| `database.go` | Postgres/ClickHouse connection helpers + auto-migration. |
| `model.go` | Shared DTOs/types used by both binaries. |
| `auth.go` | bcrypt, JWT issue/validate, constant-time dummy hash. |
| `refreshtokens.go` | Server-side refresh-token store: rotation + reuse detection. |
| `middleware.go` | Auth, AdminOnly, DeviceAuth, CORS, body-size, logging middleware. |
| `ratelimit.go` | Token-bucket limiter (per-IP on auth endpoints). |
| `ownership.go` | Shared `IsDeviceOwner` helper. |
| `mqttauth.go` | Mosquitto HTTP auth backend (`POST /mqtt/auth`) + topic ACL. |
| `oauth.go` | Google/GitHub OAuth redirect → callback → JWT. |
| `handlers.go` | Core REST handlers (auth, devices, telemetry, health, audit, plans). |
| `commands.go` | Device command queue endpoints. |
| `billing.go` | Manual invoicing + license upgrade on payment. |
| `license.go` | Plan catalog, device-cap enforcement, transactional `ClaimDevice`. |
| `export.go` | GDPR export jobs + presigned download URLs. |
| `ota.go` | OTA release management + device check endpoint. |
| `groups.go` | Per-user device groups + device tags. |
| `search.go` | Postgres full-text search (devices; audit for admins). |
| `status.go` | Online/offline detection from MQTT heartbeats + staleness sweep. |
| `alerts.go` | Alert rule evaluation, firing/resolution, notification. |
| `email.go` | Async email queue with retry/backoff. |
| `audit.go` | Audit log writer + `LogFromRequest` helper. |
| `maintenance.go` | Maintenance-mode flag + 503 middleware. |
| `retention.go` | Hourly ClickHouse retention cleanup per plan. |
| `ingest.go` | MQTT→store pipeline, device resolver, topic parsing. |
| `enricher.go` | Channel classification into PV/battery/load (pure logic). |
| `store.go` | ClickHouse batch writer (buffered channel + flush loop). |
| `websocket.go` | WebSocket hub with ownership-scoped subscriptions. |

## 6. Data flows

### 6.1 Telemetry ingest (hot path, in `ingest`)

1. Device publishes to `telemetry/{device_type}/{device_key}` with a
   `telemetry_v1` payload (`ts`, `schema`, `fw`, `rssi`, `heap_free`, `data{}`).
2. `ingest` subscribes to `telemetry/#` (QoS 1) and calls `Pipeline.Process`:
   - **Parse** the JSON payload.
   - **Resolve** the device via `PGDeviceResolver` (Postgres lookup by
     `device_key`). Unknown devices are rejected.
   - **Enrich** with `Enricher.Enrich`: classifies each channel's power into
     PV / battery charge / battery discharge / DC load using the device's
     `channel_groups` (icon + bitmask), computes `inverter_power` and a
     `system_status` enum, and rolls up min/max SoC and total energy.
   - **Store** a `TelemetryRow` into the `BatchWriter` (a buffered channel).
   - **Republish** an `EnrichedTelemetry` payload to `live/{device_key}` so the
     API can push it to browsers without re-querying ClickHouse.
3. `BatchWriter.FlushLoop` drains the buffer every 30 s (or when full) and
   writes batches to ClickHouse via `CHStore.Write`. The buffer is bounded
   (10 000 rows); when full, rows are dropped with a warning to avoid
   unbounded memory growth. On shutdown a final flush uses a fresh
   `context.WithTimeout` so a cancelled parent context can't drop the last batch.

> ClickHouse uses `?` positional placeholders (not Postgres `$n`). All
> ClickHouse queries in this codebase use `?`.

### 6.2 Live push + alerting (in `api`)

1. `api` subscribes to `live/#` (QoS 0). Each message is:
   - forwarded to `WebSocketHub.OnLiveMessage` → broadcast to subscribed
     browser sessions;
   - parsed and run through `AlertEngine.Evaluate`.
2. `AlertEngine` caches active rules (refreshed every 30 s), matches each rule
   to the device by type/key, evaluates the condition, and counts consecutive
   matches. After enough samples (`requiredSamples`) it **fires**; on a
   non-match it **resolves**. Firing is an atomic
   `INSERT ... ON CONFLICT DO NOTHING` against a partial unique index
   `(rule_id, device_key) WHERE status='firing'`, so concurrent messages cannot
   create duplicate firing events. Email is enqueued only when a row is
   actually inserted.

### 6.3 Online/offline detection (in `api`)

- Devices publish `status/{device_key}/online` with `{"online": true, "ts": …}`
  (and an MQTT LWT `{"online": false}` on disconnect).
- `api` subscribes to `status/+/online`; `DeviceStatusManager` sets
  `devices.is_active`/`last_seen_at` and broadcasts a `device_status` WebSocket
  event.
- A background sweeper (every 15 s) marks devices whose `last_seen_at` is older
  than 60 s offline and broadcasts the transition. The threshold uses
  `make_interval(secs => ?)` (a Go duration string is not valid Postgres
  `interval`).

### 6.4 Device commands (asynchronous, polled)

```
web UI ──POST /commands──▶ device_commands(pending)
device ──GET /commands/{key}/pending──▶ atomically CLAIM (FOR UPDATE SKIP LOCKED)
device ──POST /commands/{id}/result──▶ applied | failed
```

- `POST /commands` requires a user JWT and ownership of the target device.
- `GET /commands/{key}/pending` and `POST /commands/{id}/result` are
  firmware-facing: the device authenticates with its `device_key`/`api_key`
  (HTTP Basic or `X-Device-Key`/`X-Api-Key` headers). A user JWT is also
  accepted if the user owns the device. The authenticated device must match
  the command's device.

### 6.5 Auth token lifecycle

- **Access token** (HS256 JWT, default 15 m): stateless, carries `user_id` and
  `role`. Validated by `AuthMiddleware` on every protected request.
- **Refresh token** (HS256 JWT, default 720 h): **single-use and rotated**.
  Each carries a `jti` and `family_id`; every issuance is recorded in the
  `refresh_tokens` table. `RefreshTokenStore.Rotate` revokes the presented token
  and issues a new one in the same family. If a revoked/used token is presented
  again, the **entire family is revoked** (reuse detection) and the request is
  rejected — the client must re-authenticate via `/auth/login`. The new access
  token's role is looked up from Postgres at refresh time, so
  promotions/demotions take effect without a fresh login.

## 7. Data model

### 7.1 Postgres (operational state)

Migrations live in `migrations/` (`001_initial` … `006_alert_firing_unique`).

| Table | Purpose |
|---|---|
| `users` | Accounts (email, bcrypt hash, role, display_name). |
| `devices` | Registered devices (`device_key`, `api_key`, `owner_id` nullable, `is_active`, `last_seen_at`, `firmware_ver`). |
| `device_groups` / `device_group_members` | Per-user device groupings (groups have `owner_id`; members join by `device_key`). |
| `device_tags` | Key/value metadata per device. |
| `alert_rules` / `alert_events` | Rule definitions and firing/resolved events. `alert_events` has a partial unique index for active firing rows. |
| `ota_releases` | Firmware releases per device type/channel. |
| `license_plans` / `user_licenses` / `license_change_log` | Plans (free/pro/business), per-user device count + plan, audit of changes. |
| `invoices` | Manual billing; mark-paid upgrades the user's plan. |
| `notification_preferences` | Per-user alert email toggles + quiet hours. |
| `email_queue` / `email_templates` | Async outbound email with retry. |
| `audit_log` | Append-only compliance log (actor, action, resource, IP, UA, details). |
| `refresh_tokens` | Server-side refresh-token state for rotation/reuse detection. |
| `maintenance_mode` | Single-row flag read by the 503 middleware and the ingest worker. |
| `export_jobs` | GDPR export job state + MinIO object path. |
| `device_commands` | Polled command queue (pending/claimed/applied/failed). |
| `user_oauth_accounts` | OAuth identity links (provider, provider_id). |

### 7.2 ClickHouse (time-series)

- `device_telemetry`: one row per reading. Columns include `device_id`,
  `device_type`, `ts`, power fields, SoC, `system_status`, and a
  `Map(String, Float64)` `fields` column for raw device-specific values.
  Retention is enforced per-device by the ingest worker based on the owner's
  plan `retention_days`.

### 7.3 MinIO/S3

- `firmware` bucket: OTA binaries (path stored in `ota_releases.binary_path`)
  and GDPR export objects (`exports/{job_id}.json`). The `minio-init` one-shot
  creates the bucket on stack startup.

## 8. Security model

### 8.1 Authentication

- **Users**: email + password (bcrypt cost 12). JWT access tokens via
  `/auth/login` or `/auth/register`. Optional Google/GitHub OAuth.
- **Devices**: `device_key` + `api_key`. Used for MQTT auth (Mosquitto HTTP
  backend → `POST /api/v1/mqtt/auth`) and for firmware-facing command
  endpoints. Comparisons are constant-time (`crypto/subtle`).

### 8.2 Authorization

- **`AuthMiddleware`** validates the Bearer JWT and injects `user_id`/`role`
  into the request context.
- **`AdminOnly`** (applied per-route) rejects non-admins with 403. Used for
  invoice creation, mark-paid, OTA release creation, maintenance toggle.
- **Ownership scoping**: device reads/writes, telemetry, tags, group
  membership, WebSocket subscriptions, command enqueue/poll, and search results
  are all scoped to the authenticated user's devices (`IsDeviceOwner`).
  Unowned resources return 404 (existence hidden).
- **Audit log** (`/admin/audit`) returns all rows to admins and only the
  caller's rows to regular users.

### 8.3 Hardening

- **Body size**: `MaxBodySize` middleware caps every request body at 1 MiB to
  prevent JSON-decode memory exhaustion.
- **Rate limiting**: token-bucket per IP on `/auth/register` (5/min) and
  `/auth/login` (10/min); the limiter strips the ephemeral port so each
  request does not get its own bucket.
- **Login timing**: a missing user runs a dummy bcrypt comparison so the
  "user not found" and "wrong password" paths take roughly the same time
  (prevents account enumeration).
- **Refresh tokens**: single-use rotation + family revocation on reuse (§6.5).
- **OAuth**: random CSRF `state` (panics if `crypto/rand` fails); existing
  accounts are only linked by email if they have no password (prevents
  pre-hijacking); a clear error is returned if an email already belongs to a
  password account.
- **CORS**: explicit origin allowlist; `*` and `null` are stripped because they
  are unsafe with `AllowCredentials`.
- **Secrets**: `JWT_SECRET` must be ≥ 32 chars and is rejected if it is the
  shipped example placeholder.
- **SQL**: all queries use parameterized placeholders; dynamic `WHERE` clauses
  (e.g. audit filtering) build numbered `$n` placeholders, never string
  interpolation of values.

### 8.4 MQTT authorization

`POST /api/v1/mqtt/auth` returns 200 only when `device_key` + `api_key` are
valid and the device is active, and returns an ACL restricting each device to:
`telemetry/{key}/#` and `status/{key}/#` (write), `commands/{key}` and
`ota/{key}` (read). Production Mosquitto uses this HTTP auth backend via a
compiled `auth_plugin`; the dev stack uses `allow_anonymous true` for
convenience (see `mosquitto/README.md`).

## 9. Configuration

All configuration is environment-driven (`internal/config.go`); see `.env.example`
for the full list with defaults. Required: `DATABASE_URL`, `CLICKHOUSE_URL`,
`MQTT_BROKER`, `JWT_SECRET`. Notable optional ones: `MINIO_PUBLIC_URL`
(device-reachable firmware URL), `CORS_ALLOWED_ORIGINS`, OAuth credentials,
SMTP. `AUTO_MIGRATE=true` runs `golang-migrate` on startup.

## 10. Deployment

- **Dev**: `docker-compose.yml` — Postgres, ClickHouse, Mosquitto (anonymous),
  MinIO, `api`, `ingest`, and a `minio-init` one-shot. See `SETUP.md` for the
  smoke-test walkthrough.
- **Prod overlay**: `docker-compose.prod.yml` — layers Traefik (TLS-ALPN-01
  Let's Encrypt, HTTP→HTTPS redirect, WebSocket-aware routing), production
  Mosquitto (HTTP-auth plugin image, build steps in `mosquitto/README.md`),
  and daily backup sidecars (`postgres-backup`, `clickhouse-backup`,
  `minio-backup`). Service ports are not published directly except `1883`
  (firewalled to device subnets).

## 11. Observability

- **Logging**: structured `log/slog` to stdout (JSON-friendly). Request
  middleware logs method/path/status/duration. `slog.SetLogLoggerLevel` follows
  `LOG_LEVEL`.
- **Health**: `GET /api/v1/health` reports Postgres + ClickHouse reachability.
  It is whitelisted through the maintenance-mode middleware.
- **Audit log**: every security-relevant mutation is recorded
  (`user.register`, `user.login`, `device.claim`, `group.*`, `device.tag.*`,
  `export.request`, `billing.invoice.*`, `ota.release.create`,
  `admin.maintenance_toggle`) with actor, IP, user-agent, and details.
- **Maintenance mode**: a DB flag toggled by admins; the API returns 503 to all
  non-health routes and the ingest worker pauses processing while it is on.

## 12. Scaling & limitations

- **Horizontal scaling**: `api` is stateless (per-process WebSocket hub aside)
  and can run as multiple replicas behind Traefik; WebSocket sessions are
  sticky per instance (no cross-instance fan-out yet — a future Redis/pubsub
  bridge would be needed for true horizontal WS scaling). `ingest` can also
  scale, but MQTT client IDs must be unique per process and the shared
  `telemetry/#` subscription benefits from Mosquitto's shared-subscription
  support (`$share/.../telemetry/#`) to avoid duplicate processing.
- **Backpressure**: the ingest buffer is bounded; under sustained overload it
  drops rows with a warning rather than growing unbounded. Consider a
  spill-to-disk or a persistent queue for lossless ingestion.
- **ClickHouse writes**: currently one `INSERT` per row via the batch writer's
  flush loop. For higher throughput, switch `CHStore.Write` to a real batch
  insert (`PrepareBatch` / `AsyncInsert`).
- **WebSocket fan-out** is in-process only (see above).
- **Email** is best-effort and async; queue failures are retried with backoff
  but not persisted beyond the `email_queue` table.

## 13. File map (quick links)

- Source: `internal/*.go`, `cmd/{api,ingest,seed}/main.go`
- Migrations: `migrations/001_*` … `migrations/006_*`
- Config: `.env.example`, `internal/config.go`
- Deploy: `docker-compose.yml`, `docker-compose.prod.yml`, `Dockerfile.{api,ingest}`
- Broker: `mosquitto/config/*.conf`, `mosquitto/README.md`
- Docs: `docs/API.md`, `docs/SETUP.md`, `docs/ARCHITECTURE.md`, `docs/AUTH.md`,
  `docs/REALTIME.md`, `docs/ESP32-GUIDE.md`
- Generated OpenAPI: `docs/api/{docs.go,swagger.json,swagger.yaml}` (regenerate
  with `make swag`).