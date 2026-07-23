# IoT Platform Backend API Reference

Base URL: `http://localhost:8080/api/v1` (or your deployed API host).

Authentication: most endpoints require `Authorization: Bearer <jwt>`.

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

Rotate tokens using a refresh token.

```bash
curl -s -X POST http://localhost:8080/api/v1/auth/refresh \
  -H 'Content-Type: application/json' \
  -d '{"refresh_token":"..."}'
```

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

Server messages:

```json
{"type":"telemetry", ...}
{"type":"pong"}
```

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

Device-polling endpoint. No auth required.

```bash
curl -s http://localhost:8080/api/v1/ota/check/AABBCCDDEEFF?current_ver=2.0.0
```

### POST `/ota/releases` (auth required)

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

## Billing

### GET `/billing/invoices`

List invoices for the authenticated user.

### POST `/billing/invoices`

Create invoice (admin only).

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

### POST `/billing/invoices/{id}/mark-paid`

Mark an invoice paid and upgrade the user's license.

## Export

### POST `/export/request`

Start an async GDPR export. Writes a JSON file to MinIO.

### GET `/export/status/{id}`

Check export job status.

## Admin

### POST `/admin/maintenance`

Toggle maintenance mode (auth required; admin role not enforced in current implementation).

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

Full-text search across devices and audit log.

```bash
curl -s 'http://localhost:8080/api/v1/search?q=solar' \
  -H "Authorization: Bearer $TOKEN"
```
