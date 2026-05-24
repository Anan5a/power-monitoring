# REST API Reference

Base URL: `https://<project>.supabase.co`

Authentication: All endpoints require `apikey` and `Authorization: Bearer <key>` headers.

---

## Telemetry Endpoints

### POST `/rest/v1/rpc/insert_telemetry`

Insert a telemetry reading. Called by ESP32 using a per-device limited anon key (NOT service_role).

**Headers:**
```
Content-Type: application/json
apikey: <per-device anon key>
Authorization: Bearer <per-device anon key>
```

**Body:**
```json
{
  "p_device_key": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "p_device_api_key": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "p_payload": {
    "sensor_key": 12.34,
    "another_sensor": 56.78
  },
  "p_metadata": {
    "rssi": -55,
    "vcc": 3.31,
    "uptime_s": 3600
  },
  "p_recorded_at": 1750000000
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `p_device_key` | text | Yes | Device UUID (maps device to user account) |
| `p_device_api_key` | uuid | Yes | Per-device API key — validates this device can only insert its own data |
|---|---|---|---|
| `p_device_key` | text | Yes | Device key from `devices` table |
| `p_payload` | jsonb | Yes | Any `{key: value}` sensor readings |
| `p_metadata` | jsonb | No | Device metadata (RSSI, voltage, uptime) |
| `p_recorded_at` | bigint | No | Unix epoch seconds of the actual measurement. If omitted, uses server receive time. |

**Responses:**
- `200` / `204` — Success
- `400` — Invalid payload
- `401` — Invalid API key
- `400` — Invalid device_key

**Example (curl):**
```bash
curl -X POST "https://your-project.supabase.co/rest/v1/rpc/insert_telemetry" \
  -H "Content-Type: application/json" \
  -H "apikey: YOUR_SERVICE_ROLE_KEY" \
  -H "Authorization: Bearer YOUR_SERVICE_ROLE_KEY" \
  -d '{"p_device_key":"dev-key","p_payload":{"temp":25.4},"p_metadata":{}}'
```

---

### GET `/rest/v1/telemetry_live`

Fetch recent telemetry for a device.

**Query parameters:**
| Param | Example | Description |
|---|---|---|
| `device_id` | `eq.device-key` | Filter by device |
| `select` | `*` | Columns to return |
| `order` | `recorded_at.desc` | Sort order |
| `limit` | `200` | Max rows |
| `recorded_at` | `gte.2024-01-01` | Time range filter |

**Example — last 200 readings for a device:**
```bash
curl "https://your-project.supabase.co/rest/v1/telemetry_live?device_id=eq.device-key&order=recorded_at.desc&limit=200" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY"
```

**Response:**
```json
[
  {
    "id": 1,
    "device_id": "device-key",
    "recorded_at": "2024-01-01T12:00:00Z",
    "payload": {"ina3221_v0": 12.34, "ina3221_i0": 1.23},
    "metadata": {"rssi": -55}
  }
]
```

---

### GET `/rest/v1/telemetry_archive`

Fetch daily aggregated telemetry.

**Query parameters:**
| Param | Example | Description |
|---|---|---|
| `device_id` | `eq.device-key` | Filter by device |
| `log_date` | `gte.2024-01-01` | Date range |
| `order` | `log_date.desc` | Sort order |

**Response:**
```json
[
  {
    "device_id": "device-key",
    "log_date": "2024-01-01",
    "sample_count": 2880,
    "payload_agg": {"ina3221_v0_avg": "12.34", "ina3221_v0_max": "13.5"}
  }
]
```

---

## Device Endpoints

### GET `/rest/v1/devices`

Fetch all devices for the authenticated user.

```bash
curl "https://your-project.supabase.co/rest/v1/devices?select=*" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY"
```

### POST `/rest/v1/devices`

Register a new device.

**Body:**
```json
{
  "device_name": "Workshop Monitor",
  "device_type": "power-monitor",
  "device_key": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "user_id": "uuid-of-logged-in-user"
}
```

### DELETE `/rest/v1/devices`

Delete a device.

```bash
curl -X DELETE "https://your-project.supabase.co/rest/v1/devices?id=eq.device-uuid" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY"
```

---

## Relay Endpoints

### GET `/rest/v1/relay_states`

Fetch relay states for a device.

```bash
curl "https://your-project.supabase.co/rest/v1/relay_states?device_key=eq.device-key" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY"
```

### PATCH `/rest/v1/relay_states`

Toggle a relay on/off.

**Example — energize relay 0:**
```bash
curl -X PATCH "https://your-project.supabase.co/rest/v1/relay_states?device_key=eq.device-key&relay_index=eq.0" \
  -H "Content-Type: application/json" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY" \
  -d '{"is_energized": true}'
```

---

## Device Profiles

### GET `/rest/v1/device_profiles`

Fetch available device type profiles.

```bash
curl "https://your-project.supabase.co/rest/v1/device_profiles?select=*" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY"
```

**Response:**
```json
[
  {
    "device_type": "power-monitor",
    "label": "Power Monitor v2",
    "fields": [
      {"key": "ina3221_v0", "label": "Ch0 Voltage", "unit": "V", "chart": true}
    ]
  }
]
```

---

## Auth Endpoints (Supabase Built-in)

### POST `/auth/v1/token?grant_type=password`

Sign in with email/password.

```bash
curl -X POST "https://your-project.supabase.co/auth/v1/token?grant_type=password" \
  -H "Content-Type: application/json" \
  -H "apikey: YOUR_ANON_KEY" \
  -d '{"email": "user@example.com", "password": "password123"}'
```

### POST `/auth/v1/signup`

Create a new account.

```bash
curl -X POST "https://your-project.supabase.co/auth/v1/signup" \
  -H "Content-Type: application/json" \
  -H "apikey: YOUR_ANON_KEY" \
  -d '{"email": "user@example.com", "password": "password123"}'
```

### POST `/auth/v1/logout`

Sign out (requires session token).

```bash
curl -X POST "https://your-project.supabase.co/auth/v1/logout" \
  -H "Authorization: Bearer YOUR_SESSION_TOKEN" \
  -H "apikey: YOUR_ANON_KEY"
```

---

## Rate Limits (Free Tier)

| Operation | Limit |
|---|---|
| Auth requests | 60/min |
| REST reads | Unlimited |
| REST inserts (via RPC) | Unlimited |
| Realtime connections | 200 concurrent |

Exceeded limits return `429 Too Many Requests`.