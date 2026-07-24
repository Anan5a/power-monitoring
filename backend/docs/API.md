# IoT Platform Backend API Reference

Base URL: `http://localhost:8080/api/v1` (or your deployed API host).
Interactive OpenAPI/Swagger UI: `http://localhost:8080/api/v1/swagger/`.
Source for the architecture and data model: [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Conventions

### Authentication

- **User endpoints** require `Authorization: Bearer <jwt>` (an access token from
  `/auth/login`, `/auth/register`, or `/auth/refresh`). Access tokens are HS256
  JWTs (default 15 m) carrying `user_id` and `role`.
- **Admin endpoints** are marked *(admin only)*; they additionally require
  `role == "admin"` (enforced by the `AdminOnly` middleware → `403`).
- **Device (firmware) endpoints** are marked *(device auth)*; the device
  authenticates with its `device_key` / `api_key` via HTTP Basic auth or the
  `X-Device-Key` / `X-Api-Key` headers (the same credentials used for MQTT).
  A user Bearer token is also accepted where noted if the user owns the device.
- **Ownership scoping**: device, telemetry, tag, group, command, export, and
  search access is scoped to the authenticated user's devices. Unowned
  resources return `404` (their existence is hidden).

### Error format

All errors use a consistent envelope:

```json
{ "error": { "code": "forbidden", "message": "admin privileges required" } }
```

Common `code` values: `bad_request`, `validation_error`, `unauthorized`,
`forbidden`, `not_found`, `conflict`, `rate_limited`, `internal_error`.

| HTTP status | Meaning |
|---|---|
| 200 / 201 | Success (created for POSTs that make a resource). |
| 202 | Accepted — asynchronous job started (export, command enqueue uses 202). |
| 400 | Validation/parse error. |
| 401 | Missing/invalid credentials. |
| 403 | Authenticated but not allowed (e.g. non-admin on an admin route, device-key mismatch). |
| 404 | Resource not found / not owned. |
| 409 | Conflict (duplicate, already-processed). |
| 429 | Rate limited (auth endpoints only). |
| 500 | Internal error. |
| 503 | Maintenance mode is on. |

### Rate limiting

`/auth/register` is limited to 5 requests/minute/IP and `/auth/login` to
10/minute/IP. Exceeded limits return `429` with a `Retry-After` header. The
limiter keys on the client IP (port stripped).

### Request bodies

All request bodies are JSON (`Content-Type: application/json`), capped at 1 MiB.
Responses are JSON. Timestamps are RFC 3339 / ISO 8601 (UTC where device-time
isn't involved). IDs are UUIDs (Postgres) except command IDs (int64).

### Maintenance mode

When an admin enables maintenance mode, all non-health endpoints return `503`
with `{"error":"maintenance","message":"..."}` until it is disabled. The ingest
worker also pauses processing while maintenance is on.

## Auth

### POST `/auth/register`

Create a user account.

```bash
curl -s -X POST http://localhost:8080/api/v1/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"email":"user@example.com","password":"Password123!","display_name":"User"}'
```

Response:

```json
{
  "access_token": "...",
  "refresh_token": "...",
  "user": { "id": "...", "email": "user@example.com", "role": "user" }
}
```

### POST `/auth/login`

Obtain JWT tokens.

```bash
curl -s -X POST http://localhost:8080/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"email":"user@example.com","password":"Password123!"}'
```

### POST `/auth/refresh`

Rotate tokens using a refresh token. Refresh tokens are **single-use and
rotated**: each call revokes the presented token and returns a new refresh
token (in the same family). If a token that was already used/revoked is
presented again, the entire family is revoked (reuse-detection) and the
request is rejected — treat any `401` from this endpoint as "re-authenticate
with `/auth/login`". The new access token carries the user's current role
(looked up at refresh time).

```bash
curl -s -X POST http://localhost:8080/api/v1/auth/refresh \
  -H 'Content-Type: application/json' \
  -d '{"refresh_token":"..."}'
```

Response: `{ "access_token": "...", "refresh_token": "..." }`.

### OAuth

- `GET /auth/oauth/{provider}` — redirect to Google/GitHub.
- `GET /auth/oauth/{provider}/callback` — OAuth callback.

Supported providers: `google`, `github`.

## Devices

### GET `/devices`

List devices owned by the authenticated user.

```bash
curl -s http://localhost:8080/api/v1/devices -H "Authorization: Bearer $TOKEN"
```

### GET `/devices/{key}`

Get a single device.

```bash
curl -s http://localhost:8080/api/v1/devices/AABBCCDDEEFF -H "Authorization: Bearer $TOKEN"
```

### POST `/devices/{key}/claim`

Claim an unclaimed device using its API key.

```bash
curl -s -X POST http://localhost:8080/api/v1/devices/AABBCCDDEEFF/claim \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"api_key":"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}'
```

## Telemetry

### GET `/telemetry/{key}/latest`

Latest enriched telemetry snapshot from ClickHouse.

```bash
curl -s http://localhost:8080/api/v1/telemetry/AABBCCDDEEFF/latest \
  -H "Authorization: Bearer $TOKEN"
```

### MQTT ingestion

Devices publish to:

```
telemetry/{device_type}/{device_key}
```

Example payload:

```json
{
  "ts": 1720000000,
  "ts_ms": 0,
  "schema": "telemetry_v1",
  "fw": "2.0.0",
  "uptime_ms": 3600000,
  "rssi": -55,
  "heap_free": 150000,
  "data": {
    "ch0_P": 19.8,
    "ch1_P": -6.4,
    "energy_wh0": 120.5,
    "soc_pct0": 85.0
  }
}
```

The ingest worker enriches the payload and republishes to `live/{device_key}`.

## WebSocket

Connect to `ws://localhost:8080/api/v1/ws` with `Authorization: Bearer <token>`.

Client messages:

```json
{"type":"subscribe","device_keys":["AABBCCDDEEFF"]}
{"type":"unsubscribe","device_keys":["AABBCCDDEEFF"]}
{"type":"ping"}
```

Subscriptions are scoped to devices owned by the authenticated user; subscribing
to another user's device returns an error and is ignored.

```json
{"type":"telemetry", ...}
{"type":"device_status", "device_key": "...", "online": true, "ts": 1720000000}
{"type":"pong"}
```

### Online/offline detection

Devices publish a heartbeat to:

```
status/{device_key}/online
```

with payload `{"online": true, "ts": <unix_seconds>}`. The API subscribes to
`status/+/online`, sets `devices.is_active=true` and `last_seen_at=now()`, and
broadcasts a `device_status` event to subscribed WebSocket clients. A background
sweeper marks devices whose `last_seen_at` is older than 60s offline and
broadcasts the transition. Firmware should also configure an MQTT LWT
(`{"online": false}`) so disconnects are detected immediately.

## Groups & Tags

### Groups

- `GET /groups`
- `POST /groups` — `{name, description, color}`
- `POST /groups/{id}/devices/{key}` — add device
- `DELETE /groups/{id}/devices/{key}` — remove device

### Tags

- `GET /devices/{key}/tags`
- `POST /devices/{key}/tags/{tag_key}` — `{value}`
- `DELETE /devices/{key}/tags/{tag_key}`

## OTA

### GET `/ota/check/{key}?current_ver=2.0.0`

Device-polling endpoint. No auth required. The returned `binary_url` is built
from `MINIO_PUBLIC_URL` (a device-reachable base) — set it in `.env` to the
external firmware download URL.

```bash
curl -s http://localhost:8080/api/v1/ota/check/AABBCCDDEEFF?current_ver=2.0.0
```

### POST `/ota/releases` (admin only)

Create an OTA release.

```bash
curl -s -X POST http://localhost:8080/api/v1/ota/releases \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{
    "device_type": "power_monitor_v2",
    "version": "2.1.0",
    "channel": "stable",
    "binary_path": "firmware/power_monitor_v2-2.1.0.bin",
    "binary_size": 1048576,
    "sha256": "abc123...",
    "changelog": "Bug fixes"
  }'
```

## Commands

A web UI enqueues commands for a device; the device polls and reports results.
Command lifecycle: `pending` → `claimed` → `applied` | `failed`.

### POST `/commands` (auth required)

Enqueue a command for a device you own.

```bash
curl -s -X POST http://localhost:8080/api/v1/commands \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"device_key":"AABBCCDDEEFF","cmd_type":"relay.set","payload":{"relay":1,"state":1}}'
```

Response: the created `DeviceCommand` (status `pending`).

### GET `/commands/{key}/pending` (device auth)

Device-polling endpoint. The device authenticates with its `device_key` /
`api_key` (the same credentials it uses for MQTT) via either HTTP Basic auth
or the `X-Device-Key` / `X-Api-Key` headers. The authenticated device must
match the path `device_key`. A user Bearer token is also accepted if the user
owns the device. Atomically claims all `pending` commands (marks them
`claimed` via `FOR UPDATE SKIP LOCKED`) and returns them.

```bash
curl -s -u AABBCCDDEEFF:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx \
  http://localhost:8080/api/v1/commands/AABBCCDDEEFF/pending
```

```json
[
  {"id": 12, "device_key": "...", "cmd_type": "relay.set", "payload": {"relay":1,"state":1}, "status": "claimed", "created_at": "..."}
]
```

### POST `/commands/{id}/result` (device auth)

Report the outcome of a command. `status` must be `applied` or `failed`. The
command must belong to the authenticated device (or a user who owns it).

```bash
curl -s -u AABBCCDDEEFF:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx \
  -X POST http://localhost:8080/api/v1/commands/12/result \
  -H 'Content-Type: application/json' \
  -d '{"status":"applied","result":{"relay":1,"state":1}}'
```

## Billing

### GET `/billing/plans`

List the public license plan catalog. No auth required.

```bash
curl -s http://localhost:8080/api/v1/billing/plans
```

```json
[
  {"id": 1, "name": "Free", "audience": "user", "max_devices": 1, "retention_days": 7, "price_monthly": 0, "features": ["..."]}
]
```

Device-cap enforcement: `POST /devices/{key}/claim` returns `403 forbidden`
when the owner has reached their plan's `max_devices` limit.

### GET `/billing/invoices`

List invoices for the authenticated user.

### POST `/billing/invoices` (admin only)

Create invoice.

```bash
curl -s -X POST http://localhost:8080/api/v1/billing/invoices \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{
    "user_id": "...",
    "plan_id": 2,
    "audience": "user",
    "period_start": "2026-07-01",
    "period_end": "2026-07-31",
    "amount_cents": 999,
    "description": "Pro plan July 2026"
  }'
```

### POST `/billing/invoices/{id}/mark-paid` (admin only)

Mark an invoice paid and upgrade the user's license.

## Export

### POST `/export/request`

Start an async GDPR export. Writes a JSON file to MinIO.

### GET `/export/status/{id}`

Check export job status.

### GET `/export/download/{id}` (auth required)

Returns a presigned MinIO download URL for a `ready` export. The URL is valid
for 3600 seconds (1 hour). Requires the job's `status` to be `ready`.

```bash
curl -s http://localhost:8080/api/v1/export/download/<job_id> \
  -H "Authorization: Bearer $TOKEN"
```

```json
{"download_url": "https://...expires=3600", "expires_in_seconds": 3600}
```

## Admin

### GET `/admin/audit` (auth required)

Query the audit log. Admins see all entries; non-admins see only their own.
Filters: `action`, `resource_type`, `limit` (default 50), `offset`.

```bash
curl -s 'http://localhost:8080/api/v1/admin/audit?action=device.claim&limit=20' \
  -H "Authorization: Bearer $TOKEN"
```

Audited actions include `user.register`, `user.login`, `device.claim`,
`group.create`, `group.add_device`, `group.remove_device`, `device.tag.set`,
`device.tag.delete`, `export.request`, `billing.invoice.create`,
`billing.invoice.paid`, `ota.release.create`, `admin.maintenance_toggle`.

### POST `/admin/maintenance` (admin only)

Toggle maintenance mode.

```bash
curl -s -X POST http://localhost:8080/api/v1/admin/maintenance \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"enabled":true,"message":"Upgrading database"}'
```

## Health

### GET `/health`

No auth required.

```bash
curl -s http://localhost:8080/api/v1/health
```

## Search

### GET `/search?q=solar`

Full-text search. Devices are always scoped to the authenticated user. Audit
results are returned **only to admins** (`type=audit`); non-admins get device
results only. Query params: `q` (required), `type` (`devices`, `audit`,
comma-separated; defaults to `devices`), `limit` (default 20), `offset`.

```bash
curl -s 'http://localhost:8080/api/v1/search?q=solar' \
  -H "Authorization: Bearer $TOKEN"
```

## Notifications

Per-user notification preferences for alert emails.

### GET `/users/me/notifications` (auth required)

Returns the user's preferences. Defaults (all enabled, no quiet hours) are
returned if the user has never saved any.

```bash
curl -s http://localhost:8080/api/v1/users/me/notifications \
  -H "Authorization: Bearer $TOKEN"
```

```json
{ "alert_fired_email": true, "alert_resolved_email": true, "quiet_hours_start": null, "quiet_hours_end": null }
```

### PATCH `/users/me/notifications` (auth required)

Update preferences. All fields optional; omitted booleans are preserved.

```bash
curl -s -X PATCH http://localhost:8080/api/v1/users/me/notifications \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"alert_fired_email":true,"alert_resolved_email":false,"quiet_hours_start":22,"quiet_hours_end":7}'
```

## MQTT

### POST `/mqtt/auth` (Mosquitto backend)

Called by Mosquitto's HTTP auth plugin on every device CONNECT. The device's
MQTT username is its `device_key` and password is its `api_key`. Returns `200`
with ACLs on success, `403` on failure. Not intended for direct client use.

```json
// request
{ "username": "AABBCCDDEEFF", "password": "xxxxxxxx-xxxx-...", "topic": "...", "acc": "publish" }
// 200 response
{ "ok": true, "acl": [ { "topic": "telemetry/AABBCCDDEEFF/#", "access": "write" },
                       { "topic": "status/AABBCCDDEEFF/#", "access": "write" },
                       { "topic": "commands/AABBCCDDEEFF", "access": "read" },
                       { "topic": "ota/AABBCCDDEEFF", "access": "read" } ] }
// 403 response
{ "ok": false }
```

### MQTT topics (device ↔ broker)

| Topic | Direction | Purpose |
|---|---|---|
| `telemetry/{device_type}/{device_key}` | device → broker | Raw telemetry publish (QoS 1). |
| `status/{device_key}/online` | device → broker | Online heartbeat / LWT offline. |
| `live/{device_key}` | ingest → broker/api | Enriched telemetry republished for live push. |
| `commands/{device_key}` | (reserved) | Command delivery topic space. |

See [`ARCHITECTURE.md`](ARCHITECTURE.md) §6 for the full data-flow diagrams.
