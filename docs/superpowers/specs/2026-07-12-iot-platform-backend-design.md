# IoT Platform Backend Design

**Date:** 2026-07-12
**Status:** Draft

## Overview

A self-hosted IoT platform backend replacing Supabase for the power-monitoring ecosystem. Designed for multi-tenant SaaS with support for multiple device types, OTA firmware updates, and dual licensing (manufacturer + end-user).

## Stack

| Component | Technology | Role |
|---|---|---|
| API Server | Go + chi router (`cmd/api`) | Auth, REST, WebSocket, OTA check, Mosquitto auth backend, **alert engine, email service**, admin, billing |
| Ingest Worker | Go (`cmd/ingest`) | MQTT consumer, parse/validate/enrich/store, batch writer — **pure data plumbing, no business logic** |
| MQTT Broker | Mosquitto 2 | Device ingestion, command delivery, live fan-out bus |
| Relational DB | PostgreSQL 16 | Users, orgs, devices, configs, commands, alerts, email templates, invoices, audit |
| Time-Series DB | ClickHouse 24.3 | Device telemetry, materialized views |
| Object Storage | MinIO | Firmware binaries for OTA, data exports |
| Web UI | React/Vite (existing) | Dashboard, device management, admin, billing |
| Reverse Proxy | Caddy | HTTPS, auto Let's Encrypt |

### Service Split

The API server and ingest worker are **separate binaries** sharing one Go module and the `internal/` packages. They fail independently: an ingest panic (e.g. malformed payload from a new device type) does not take down login or the dashboard, and an API restart does not pause telemetry storage (Mosquitto buffers).

**Principle: ingest is pure data plumbing.** It parses, validates, enriches, stores, and republishes — nothing else. All business logic (alerts, email, billing, OTA, auth) lives in the API. This keeps the 5s ingestion path fast and reliable: a bad alert rule or a slow SMTP server can never block or endanger telemetry storage.

```
ESP32s ──MQTT──▶ Mosquitto ──telemetry/#──▶ ingest worker
                      │                          ├─ parse/validate/enrich
                      │                          ├─ write ClickHouse (batched)
                      │                          ├─ write PG (last_seen)
                      │                          └─ republish live/{device_key} ──┐
                      │                                                          │
                      └──live/#──▶ api server ◀──────────────────────────────────┘
                      │              ├─ WebSocket push to browsers
                      │              ├─ alert rule evaluation (on live/# stream)
                      │              ├─ async email (auth, alerts, billing)
                      │              ├─ REST (auth, devices, OTA check, admin)
                      │              └─ Mosquitto auth backend (validates device creds)
                      │
Web UI ──HTTP/WS──▶ Caddy ──▶ api server
```

**Live stream does double duty:** the API subscribes to `live/#` once and uses each message for both WebSocket fan-out AND alert evaluation. The ingest worker republishes the full enriched payload (computed columns + raw fields), so the API has everything alert eval needs without re-querying.

**Email is async:** the API pushes email jobs to a `email_queue` table; a background goroutine drains it and sends via SMTP with retry. SMTP latency (100ms-2s) never blocks a request or the alert eval loop. All email triggers (welcome, password reset, alert fired/resolved, payment confirmed) originate in the API.

**Shared state (no write conflicts):**
- API writes: `device_commands`, `alert_rules`, `alert_events`, `ota_releases`, `invoices`, `users`, `orgs`, `email_queue`, `audit_log` (user actions)
- Ingest writes: `devices.last_seen_at`, ClickHouse `device_telemetry`, `audit_log` (device/system actions: validation_failed, offline)
- API reads: telemetry from ClickHouse, alert events, device list
- Ingest reads: `devices`, `device_config`, `license_plans` (via 5-min in-memory cache). Ingest does NOT read `alert_rules` — alerts are the API's job.

**Team ownership (2-3 people):**
- API owner: auth, REST, WebSocket hub, alert engine, email service, OTA, billing, Mosquitto auth endpoint (all business logic)
- Ingest owner: MQTT subscription, pipeline, enricher, batch writer, retention cleanup (pure plumbing — small, reliable, rarely changes after Phase 1)
- Infra/UI owner: Docker, CI/CD, backups, Caddy, web UI migration

## Architecture

```
                    ┌─────────────────────┐
                    │      Web UI         │
                    │  (React SPA)        │
                    └──────┬──────┬───────┘
                           │      │
                    REST   │      │ WebSocket
                           │      │
                    ┌──────▼──────▼───────┐
                    │   API Server (Go)    │  cmd/api
                    │  ┌────────────────┐  │
                    │  │ Auth (JWT)     │  │
                    │  ├────────────────┤  │
                    │  │ REST handlers  │  │
                    │  ├────────────────┤  │
                    │  │ WebSocket Hub  │  │  ← subscribes live/# from Mosquitto
                    │  ├────────────────┤  │
                    │  │ Alert Engine   │  │  ← evaluates rules on live/# stream
                    │  ├────────────────┤  │
                    │  │ Email Service  │  │  ← async queue, SMTP
                    │  ├────────────────┤  │
                    │  │ OTA check      │  │
                    │  ├────────────────┤  │
                    │  │ Mosquitto auth │  │  ← validates device creds (HTTP)
                    │  ├────────────────┤  │
                    │  │ Admin/Billing  │  │
                    │  └────────────────┘  │
                    └──────┬──────┬─────────┘
                           │      │
              ┌────────────┘      └──────────────┐
              │                                   │
     ┌────────▼──────┐  ┌──────────▼──────┐  ┌────▼──────┐
     │   PostgreSQL  │  │   ClickHouse    │  │  MinIO    │
     │  (shared)     │  │   (shared)      │  │ (S3)      │
     │  users        │  │  device_telemetry│  │ firmware  │
     │  orgs         │  │  MVs            │  │ exports   │
     │  devices      │  │  TTL            │  │           │
     │  ...          │  └─────────────────┘  └───────────┘
     └────────▲─────┘
              │
     ┌────────┴─────┐         ┌─────────────┐
     │  Ingest      │◀──MQTT──│  Mosquitto  │◀── ESP32s
     │  Worker (Go) │ telemetry│             │
     │  cmd/ingest  │  #       │  status/#   │
     │  ┌─────────┐ │   live/# │             │
     │  │ MQTT    │─┼────────▶│ (republish) │
     │  │ consumer│ │          └──────┬──────┘
     │  ├─────────┤ │                 │ live/#
     │  │ Pipeline│ │                 ▼
     │  │ ├parse  │ │          ┌─────────────┐
     │  │ ├valid. │ │          │  API Server  │ (subscribes live/# →
     │  │ ├enrich │ │          │              │  WebSocket + alert eval)
     │  │ └store  │ │          └─────────────┘
     │  ├─────────┤ │
     │  │ Batch   │ │
     │  │ writer  │ │
     │  └─────────┘ │
     └──────────────┘
```

## Data Flow

### Device Registration & Claiming

```
Factory → First boot → Owned by user

1. ESP32 flashes firmware containing device_type (set at manufacture time)
2. ESP32 derives device_key from MAC address (deterministic, unique per chip)
3. ESP32 generates api_key (random UUID, stored in NVS on first boot)
4. ESP32 connects to MQTT with device_key + api_key
   ├─ Mosquitto HTTP auth → Go API validates
   │   ├─ If device unknown → Go API auto-registers it:
   │   │     INSERT INTO devices (device_key, device_type, owner_id=NULL, api_key=...)
   │   │     status = "unclaimed"
   │   └─ If device known → validate api_key matches, allow connect
5. ESP32 begins publishing telemetry (owner_id still NULL)
   ├─ Go API stores telemetry in ClickHouse (device_id = device_key)
   └─ Telemetry is visible to admins, not yet to end users
6. End user logs into web UI → sees "Unclaimed devices" list
   └─ POST /api/devices/{key}/claim { api_key: "..." }
      ├─ User enters api_key printed on device label / shown via BLE
      ├─ Go API validates api_key matches the device
      ├─ UPDATE devices SET owner_id = user.id WHERE device_key = $1
      └─ User now sees telemetry, can configure, receive alerts

7. (Optional) Device transfer:
   └─ Admin unclaims device → new user claims it
```

### Device Online/Offline Detection

```
ESP32 uses MQTT LWT (Last Will & Testament):

1. On connect, ESP32 registers LWT:
   topic = status/{device_key}/online
   payload = "0"  (retained)
   QoS = 1

2. On successful connect, ESP32 immediately publishes:
   topic = status/{device_key}/online
   payload = "1"  (retained)

3. Go API subscribes to status/+/online:
   ├─ "1" received → UPDATE devices SET is_active=true, last_seen_at=now()
   │                → WebSocket: {"type":"device_status","device_key":"...","online":true}
   └─ "0" received (LWT fired) → UPDATE devices SET is_active=false
                                 → WebSocket: {"type":"device_status","device_key":"...","online":false}
                                 → audit: device.offline

4. Staleness backstop (goroutine, every 60s):
   UPDATE devices SET is_active=false
   WHERE is_active=true AND last_seen_at < now() - interval '2 minutes'
   Catches devices whose LWT didn't fire (power loss, network drop without clean disconnect).
```

### Telemetry Ingestion

```
ESP32 (every 5s)
  │
  └─ MQTT → telemetry/{device_type}/{device_key}
      └─ Mosquitto → Go API (MQTT subscriber)
          ├─ 1. Parse JSON payload
          ├─ 2. Resolve device (cache → PostgreSQL)
          ├─ 3. Validate (schema, ranges, timestamp)
          ├─ 4. Enrich (channel classification, system status, battery SoC)
          ├─ 5. License check (device cap, retention, features)
          ├─ 6. Batch buffer (Go channel, flush 30s/1000 rows)
          │   ├─ ClickHouse (telemetry)
          │   └─ PostgreSQL (last_seen, relay_states, battery_state)
          ├─ 7. Evaluate alerts (check rules, fire/resolve events)
          ├─ 8. Send email notifications (if alert fired/resolved)
          ├─ 9. Fan-out (WebSocket subscribers)
          └─ 10. Audit (state changes only, not every 5s ping)
```

### OTA Update

```
1. Org creates release → POST /api/ota/releases
2. Go API stores binary in MinIO, creates ota_releases row
3. ESP32 polls → GET /api/ota/check/{key}?current_ver=2.0.0
4. Go API checks ota_releases for matching device_type + channel
5. Returns { update_available, url, version, sha256 } or 204
6. ESP32 downloads binary, applies, reports result
7. Go API logs to audit_log
```

### Device Commands

```
1. Web UI → POST /api/commands { device_key, cmd_type, payload }
2. Go API inserts into device_commands table
3. ESP32 polls → GET /api/commands/{key}/pending
4. Go API claims oldest pending command (atomic UPDATE)
5. ESP32 applies command, reports result
6. Go API updates status, logs to audit_log
```

### Alert Evaluation

```
On every telemetry ingestion (after enrich):
  │
  ├─ 1. Load active alert rules for this device's org
  ├─ 2. For each rule:
  │     ├─ Evaluate condition against enriched payload
  │     │   e.g., "pv_power > 5000 for 5 consecutive samples"
  │     ├─ If condition met AND not already firing:
  │     │   ├─ Create alert_event (status=firing)
  │     │   ├─ Send email notification to org members
  │     │   └─ Log to audit_log
  │     └─ If condition cleared AND was firing:
  │         ├─ Update alert_event (status=resolved)
  │         ├─ Send resolved notification email
  │         └─ Log to audit_log
  │
  └─ 3. Periodic cleanup: auto-resolve stale firing alerts (>24h)
```

### OAuth / SSO Flow

```
# Built-in providers (Google, GitHub)
1. User clicks "Login with Google" → GET /api/auth/oauth/google
2. Go API redirects to Google OAuth consent screen
3. User authorizes → Google redirects to /api/auth/oauth/google/callback
4. Go API exchanges code for token, fetches user info
5. If email matches existing user → link account, issue JWT
6. If new user → create account, issue JWT
7. Returns JWT + redirect to dashboard

# Org SSO (custom OIDC provider)
1. Org admin configures SSO → POST /api/orgs/{id}/sso
   { issuer_url, client_id, client_secret, provider_name }
2. User visits /api/auth/sso/{org_slug}
3. Go API looks up org's OIDC provider, redirects to IdP
4. User authenticates with their corporate IdP
5. Callback → Go API validates ID token
6. If email domain matches org → link to org membership, issue JWT
7. If not → 403 Forbidden
```

### Maintenance Mode

```
Admin toggles: POST /api/admin/maintenance { enabled: true, message: "..." }

Effect:
  ├─ All REST endpoints (except /api/health) return 503
  │   { "error": "maintenance", "message": "Upgrading database..." }
  ├─ MQTT consumer pauses ingestion (messages queue in Mosquitto)
  ├─ WebSocket server sends "maintenance" event to connected clients
  └─ Health check still returns 200 (so load balancer keeps routing)

Admin toggles off: POST /api/admin/maintenance { enabled: false }
  ├─ REST endpoints resume
  ├─ MQTT consumer resumes processing queued messages
  └─ WebSocket clients notified "maintenance_end"
```

### Email Sending

```
Triggered by (all in the API):
  ├─ Auth: welcome email, password reset, email verification
  ├─ Alerts: alert fired, alert resolved (via alert engine on live/# stream)
  └─ Billing: payment confirmed (via invoice mark-paid handler)

Flow (async, queue-based):
  1. Triggering code calls EmailService.Enqueue / EnqueueAlert
     ├─ Checks notification preferences + quiet hours (for alerts)
     └─ INSERTs a row into email_queue (status=queued)
  2. Background DrainLoop (goroutine in API) polls every 5s:
     ├─ Claims due rows with FOR UPDATE SKIP LOCKED (multi-instance safe)
     ├─ Loads template from email_templates, renders with html/template
     ├─ Sends via SMTP (SendGrid/Mailgun)
     ├─ On success: status=sent, sent_at=now()
     └─ On failure: retry with exponential backoff (1m, 5m, 25m), then status=failed
  3. Audit log records email.sent / email.failed
```

## Data Model

### PostgreSQL Schema

```sql
-- ============================================================
-- Users & Auth
-- ============================================================
CREATE TABLE users (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email         TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    display_name  TEXT,
    role          TEXT NOT NULL DEFAULT 'user',  -- 'user', 'admin'
    created_at    TIMESTAMPTZ DEFAULT now(),
    updated_at    TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Organizations (manufacturers / hardware vendors)
-- ============================================================
CREATE TABLE organizations (
    id                UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name              TEXT NOT NULL,
    slug              TEXT UNIQUE NOT NULL,
    brand_name        TEXT,
    logo_url          TEXT,
    stripe_customer_id TEXT,
    created_at        TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE org_members (
    org_id       UUID REFERENCES organizations(id) ON DELETE CASCADE,
    user_id      UUID REFERENCES users(id) ON DELETE CASCADE,
    role         TEXT NOT NULL DEFAULT 'admin',  -- 'owner', 'admin', 'developer'
    invited_by   UUID REFERENCES users(id),
    created_at   TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (org_id, user_id)
);

-- ============================================================
-- Device Types (defined by orgs)
-- ============================================================
CREATE TABLE device_types (
    id              SERIAL PRIMARY KEY,
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    type_key        TEXT NOT NULL,  -- 'power_monitor_v2'
    display_name    TEXT NOT NULL,
    description     TEXT,
    default_config  JSONB,         -- channel groups, calibration defaults
    schema_def      JSONB,         -- expected fields, validation rules
    created_at      TIMESTAMPTZ DEFAULT now(),
    UNIQUE (org_id, type_key)
);

-- ============================================================
-- Devices (owned by users, manufactured by orgs)
-- ============================================================
CREATE TABLE devices (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_key    TEXT UNIQUE NOT NULL,
    device_name   TEXT NOT NULL DEFAULT 'Unnamed Device',
    device_type   TEXT NOT NULL,
    owner_id      UUID REFERENCES users(id),  -- NULL = unclaimed (auto-registered on first MQTT connect)
    org_id        UUID REFERENCES organizations(id),
    api_key       UUID UNIQUE DEFAULT gen_random_uuid(),
    is_active     BOOLEAN DEFAULT true,
    firmware_ver  TEXT,
    last_seen_at  TIMESTAMPTZ,
    created_at    TIMESTAMPTZ DEFAULT now(),
    updated_at    TIMESTAMPTZ DEFAULT now()
);

CREATE INDEX idx_devices_owner ON devices (owner_id) WHERE owner_id IS NOT NULL;
CREATE INDEX idx_devices_unclaimed ON devices (created_at DESC) WHERE owner_id IS NULL;

-- ============================================================
-- Device Commands
-- ============================================================
CREATE TABLE device_commands (
    id            BIGSERIAL PRIMARY KEY,
    device_key    TEXT NOT NULL REFERENCES devices(device_key),
    cmd_type      TEXT NOT NULL,
    payload       JSONB NOT NULL DEFAULT '{}',
    status        TEXT NOT NULL DEFAULT 'pending',  -- pending, applied, failed
    result        JSONB,                            -- device-reported result payload
    error         TEXT,                             -- device-reported error message
    created_at    TIMESTAMPTZ DEFAULT now(),
    applied_at    TIMESTAMPTZ
);

CREATE INDEX idx_commands_device_status ON device_commands (device_key, status);

-- ============================================================
-- Device Config (per-device settings, synced from device)
-- ============================================================
CREATE TABLE device_config (
    device_key      TEXT PRIMARY KEY REFERENCES devices(device_key),
    channel_groups  JSONB,
    channel_names   JSONB,
    battery_profiles JSONB,
    calibration     JSONB,
    updated_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- OTA Releases
-- ============================================================
CREATE TABLE ota_releases (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    device_type     TEXT NOT NULL,
    version         TEXT NOT NULL,  -- semver
    channel         TEXT NOT NULL DEFAULT 'stable',  -- stable, beta, canary
    binary_path     TEXT NOT NULL,  -- MinIO path
    binary_size     INT NOT NULL,
    sha256          TEXT NOT NULL,
    changelog       TEXT,
    rollout_pct     INT DEFAULT 100,
    is_rollback     BOOLEAN DEFAULT false,
    created_at      TIMESTAMPTZ DEFAULT now(),
    UNIQUE (org_id, device_type, version)
);

-- ============================================================
-- Licensing
-- ============================================================
CREATE TABLE license_plans (
    id              SERIAL PRIMARY KEY,
    name            TEXT UNIQUE NOT NULL,  -- 'free', 'pro', 'business', 'enterprise'
    audience        TEXT NOT NULL,          -- 'user' or 'org'
    max_devices     INT NOT NULL DEFAULT 1,
    retention_days  INT NOT NULL DEFAULT 7,
    features        TEXT[] NOT NULL DEFAULT '{}',
    price_monthly   INT NOT NULL DEFAULT 0  -- cents, 0 = free
);

CREATE TABLE user_licenses (
    user_id       UUID PRIMARY KEY REFERENCES users(id),
    plan_id       INT NOT NULL REFERENCES license_plans(id),
    device_count  INT NOT NULL DEFAULT 0,
    starts_at     TIMESTAMPTZ DEFAULT now(),
    expires_at    TIMESTAMPTZ,
    updated_at    TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE org_licenses (
    org_id        UUID PRIMARY KEY REFERENCES organizations(id),
    plan_id       INT NOT NULL REFERENCES license_plans(id),
    device_count  INT NOT NULL DEFAULT 0,
    starts_at     TIMESTAMPTZ DEFAULT now(),
    expires_at    TIMESTAMPTZ,
    updated_at    TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Billing / Invoicing (manual until Stripe integration)
-- ============================================================
CREATE TABLE payment_methods (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID REFERENCES organizations(id),
    user_id         UUID REFERENCES users(id),
    method_type     TEXT NOT NULL DEFAULT 'manual',  -- 'manual', 'bank_transfer', 'stripe' (future)
    label           TEXT,              -- e.g., "Bank Transfer - ABC Bank"
    instructions    TEXT,              -- payment instructions shown to customer
    is_default      BOOLEAN DEFAULT false,
    stripe_id       TEXT,              -- future: Stripe payment method ID
    created_at      TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE invoices (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID REFERENCES organizations(id),
    user_id         UUID REFERENCES users(id),
    invoice_number  TEXT UNIQUE NOT NULL,  -- e.g., "INV-2026-0001"
    description     TEXT NOT NULL,

    -- What this invoice is for
    plan_id         INT REFERENCES license_plans(id),
    audience        TEXT NOT NULL,          -- 'user' or 'org'
    period_start    DATE NOT NULL,
    period_end      DATE NOT NULL,

    -- Amounts (cents)
    amount_cents    INT NOT NULL,
    tax_cents       INT DEFAULT 0,
    total_cents     INT NOT NULL,
    currency        TEXT NOT NULL DEFAULT 'USD',

    -- Status
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending, paid, cancelled, refunded
    paid_at         TIMESTAMPTZ,
    paid_via        TEXT,                -- 'manual', 'bank_transfer', 'stripe' (future)
    notes           TEXT,                -- admin notes

    -- References
    payment_method_id UUID REFERENCES payment_methods(id),
    stripe_invoice_id  TEXT,             -- future: Stripe invoice ID
    stripe_payment_intent_id TEXT,       -- future

    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

CREATE INDEX idx_invoices_org ON invoices (org_id, created_at DESC);
CREATE INDEX idx_invoices_user ON invoices (user_id, created_at DESC);
CREATE INDEX idx_invoices_status ON invoices (status);

-- ============================================================
-- License Change Log (track plan changes for billing audit)
-- ============================================================
CREATE TABLE license_change_log (
    id              BIGSERIAL PRIMARY KEY,
    org_id          UUID REFERENCES organizations(id),
    user_id         UUID REFERENCES users(id),
    audience        TEXT NOT NULL,          -- 'user' or 'org'
    from_plan_id    INT REFERENCES license_plans(id),
    to_plan_id      INT NOT NULL REFERENCES license_plans(id),
    reason          TEXT NOT NULL,          -- 'manual_upgrade', 'payment_received', 'expired', 'admin_change'
    invoice_id      UUID REFERENCES invoices(id),
    changed_by      UUID REFERENCES users(id),  -- admin who made the change
    created_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Alert Rules (defined per org, evaluated on every ingest)
-- ============================================================
CREATE TABLE alert_rules (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,
    description     TEXT,
    device_type     TEXT,              -- NULL = applies to all device types
    device_id       TEXT,              -- NULL = applies to all devices in org
    enabled         BOOLEAN DEFAULT true,

    -- Condition: field, operator, threshold
    -- e.g., field="pv_power", op="gt", value=5000
    field           TEXT NOT NULL,      -- payload field to evaluate
    operator        TEXT NOT NULL,      -- 'gt', 'lt', 'gte', 'lte', 'eq', 'neq'
    value           FLOAT NOT NULL,

    -- Duration: how long the condition must hold before firing
    -- 0 = fire immediately on first match
    duration_sec    INT NOT NULL DEFAULT 0,

    -- Notification
    notify_email    BOOLEAN DEFAULT true,
    notify_webhook  BOOLEAN DEFAULT false,
    webhook_url     TEXT,

    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Alert Events (firing history)
-- ============================================================
CREATE TABLE alert_events (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    rule_id         UUID NOT NULL REFERENCES alert_rules(id) ON DELETE CASCADE,
    device_key      TEXT NOT NULL,
    status          TEXT NOT NULL DEFAULT 'firing',  -- 'firing', 'resolved', 'acknowledged'
    fired_at        TIMESTAMPTZ DEFAULT now(),
    resolved_at     TIMESTAMPTZ,
    fired_value     FLOAT,             -- the value that triggered the alert
    resolved_value  FLOAT,             -- the value when it resolved
    notified_at     TIMESTAMPTZ
);

CREATE INDEX idx_alert_events_rule_status ON alert_events (rule_id, status);
CREATE INDEX idx_alert_events_device ON alert_events (device_key, fired_at DESC);

-- ============================================================
-- Email Templates (editable via dashboard)
-- ============================================================
CREATE TABLE email_templates (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    template_key    TEXT UNIQUE NOT NULL,  -- 'welcome', 'password_reset', 'alert_fired', 'alert_resolved'
    subject         TEXT NOT NULL,          -- Go template, e.g., "Alert: {{.RuleName}}"
    body_text       TEXT NOT NULL,          -- Plain text Go template
    body_html       TEXT NOT NULL,          -- HTML Go template
    variables       TEXT[] NOT NULL DEFAULT '{}',  -- documented variables for the template
    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Email Queue (async send — drained by API background worker)
-- Decouples SMTP latency from request handlers and alert eval.
-- ============================================================
CREATE TABLE email_queue (
    id              BIGSERIAL PRIMARY KEY,
    template_key    TEXT NOT NULL,          -- references email_templates.template_key
    recipient       TEXT NOT NULL,          -- email address
    user_id         UUID REFERENCES users(id) ON DELETE SET NULL,  -- for pref/quiet-hours checks
    data            JSONB NOT NULL DEFAULT '{}',  -- template variables
    status          TEXT NOT NULL DEFAULT 'queued',  -- queued, sending, sent, failed
    attempts        INT NOT NULL DEFAULT 0,
    last_error      TEXT,
    next_attempt_at TIMESTAMPTZ DEFAULT now(),
    queued_at       TIMESTAMPTZ DEFAULT now(),
    sent_at         TIMESTAMPTZ
);

CREATE INDEX idx_email_queue_due ON email_queue (status, next_attempt_at)
    WHERE status IN ('queued', 'sending');

-- Seed default templates
INSERT INTO email_templates (template_key, subject, body_text, body_html, variables) VALUES
('welcome',
 'Welcome to {{.PlatformName}}',
 'Hi {{.DisplayName}},\n\nWelcome to {{.PlatformName}}! ...',
 '<h1>Welcome</h1><p>Hi {{.DisplayName}},</p>...',
 ARRAY['DisplayName', 'PlatformName', 'DashboardURL']),

('password_reset',
 'Reset your {{.PlatformName}} password',
 'Click the link to reset your password: {{.ResetURL}}',
 '<p>Click <a href="{{.ResetURL}}">here</a> to reset your password.</p>',
 ARRAY['ResetURL', 'PlatformName', 'ExpiresInMinutes']),

('alert_fired',
 'ALERT: {{.RuleName}} — {{.DeviceName}}',
 'Alert "{{.RuleName}}" fired for device {{.DeviceName}} ({{.DeviceKey}}).\nValue: {{.Value}} {{.Unit}}\nThreshold: {{.Threshold}} {{.Unit}}',
 '<h2>Alert: {{.RuleName}}</h2><p>Device: <strong>{{.DeviceName}}</strong> ({{.DeviceKey}})</p><p>Value: {{.Value}} {{.Unit}}</p><p>Threshold: {{.Threshold}} {{.Unit}}</p>',
 ARRAY['RuleName', 'DeviceName', 'DeviceKey', 'Value', 'Threshold', 'Unit', 'DashboardURL']),

('alert_resolved',
 'RESOLVED: {{.RuleName}} — {{.DeviceName}}',
 'Alert "{{.RuleName}}" for device {{.DeviceName}} has resolved.\nValue: {{.Value}} {{.Unit}}',
 '<h2>Resolved: {{.RuleName}}</h2><p>Device: <strong>{{.DeviceName}}</strong></p><p>Value: {{.Value}} {{.Unit}}</p>',
 ARRAY['RuleName', 'DeviceName', 'DeviceKey', 'Value', 'Unit', 'DashboardURL']);

-- ============================================================
-- OAuth Accounts (link social login to user)
-- ============================================================
CREATE TABLE user_oauth_accounts (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    provider        TEXT NOT NULL,          -- 'google', 'github', 'oidc'
    provider_id     TEXT NOT NULL,          -- user ID from the provider
    email           TEXT,
    display_name    TEXT,
    avatar_url      TEXT,
    created_at      TIMESTAMPTZ DEFAULT now(),
    UNIQUE (provider, provider_id)
);

CREATE INDEX idx_oauth_user ON user_oauth_accounts (user_id);

-- ============================================================
-- Org OAuth Providers (SSO — orgs configure their own IdP)
-- ============================================================
CREATE TABLE org_oauth_providers (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    provider_name   TEXT NOT NULL,          -- display name, e.g., "Company SSO"
    provider_type   TEXT NOT NULL DEFAULT 'oidc',  -- 'oidc', 'google_workspace', 'azure_ad'
    issuer_url      TEXT NOT NULL,          -- OIDC issuer URL
    client_id       TEXT NOT NULL,
    client_secret   TEXT NOT NULL,
    scopes          TEXT[] NOT NULL DEFAULT ARRAY['openid','profile','email'],
    enabled         BOOLEAN DEFAULT true,
    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Maintenance Mode (toggled via admin API)
-- ============================================================
CREATE TABLE maintenance_mode (
    id              SERIAL PRIMARY KEY,
    enabled         BOOLEAN NOT NULL DEFAULT false,
    message         TEXT DEFAULT 'Platform is under maintenance. Please check back shortly.',
    updated_by      UUID REFERENCES users(id),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

-- Seed: one row, always updated in place
INSERT INTO maintenance_mode (enabled, message) VALUES (false, '');

-- ============================================================
-- Device Groups (org-level device organization)
-- ============================================================
CREATE TABLE device_groups (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    name            TEXT NOT NULL,
    description     TEXT,
    color           TEXT,              -- hex color for UI badge
    created_at      TIMESTAMPTZ DEFAULT now(),
    UNIQUE (org_id, name)
);

CREATE TABLE device_group_members (
    group_id        UUID REFERENCES device_groups(id) ON DELETE CASCADE,
    device_key      TEXT REFERENCES devices(device_key) ON DELETE CASCADE,
    added_at        TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (group_id, device_key)
);

CREATE INDEX idx_group_members_device ON device_group_members (device_key);

-- ============================================================
-- Device Tags (simple key-value per device)
-- ============================================================
CREATE TABLE device_tags (
    device_key      TEXT REFERENCES devices(device_key) ON DELETE CASCADE,
    key             TEXT NOT NULL,
    value           TEXT NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (device_key, key)
);

-- ============================================================
-- Notification Preferences (per-user)
-- ============================================================
CREATE TABLE notification_preferences (
    user_id             UUID PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    alert_fired_email   BOOLEAN DEFAULT true,
    alert_resolved_email BOOLEAN DEFAULT true,
    daily_digest        BOOLEAN DEFAULT false,
    digest_hour         INT DEFAULT 8,           -- UTC hour to send digest
    quiet_hours_start   INT,                     -- UTC hour, e.g., 22 (10pm)
    quiet_hours_end     INT,                     -- UTC hour, e.g., 7 (7am)
    created_at          TIMESTAMPTZ DEFAULT now(),
    updated_at          TIMESTAMPTZ DEFAULT now()
);

-- Seed defaults for all users (trigger on user creation)
CREATE OR REPLACE FUNCTION handle_new_user()
RETURNS TRIGGER LANGUAGE plpgsql SECURITY DEFINER AS $$
BEGIN
    INSERT INTO notification_preferences (user_id) VALUES (NEW.id);
    RETURN NEW;
END;
$$;
CREATE TRIGGER on_user_created
    AFTER INSERT ON users
    FOR EACH ROW EXECUTE FUNCTION handle_new_user();

-- ============================================================
-- Data Export Jobs (GDPR data portability)
-- ============================================================
CREATE TABLE export_jobs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending, running, ready, failed
    format          TEXT NOT NULL DEFAULT 'csv',       -- 'csv' or 'json'
    file_path       TEXT,                              -- MinIO path when ready
    row_count       INT,
    error           TEXT,
    requested_at    TIMESTAMPTZ DEFAULT now(),
    completed_at    TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ                         -- download link expires
);

CREATE INDEX idx_export_jobs_user ON export_jobs (user_id, requested_at DESC);

-- ============================================================
-- Audit Log (must be defined before FTS section that alters it)
-- actor_id is nullable: system actions (alert.fired, maintenance)
-- have no human actor. device actions store the device's owner_id.
-- ============================================================
CREATE TABLE audit_log (
    id            BIGSERIAL PRIMARY KEY,
    org_id        UUID REFERENCES organizations(id),
    actor_id      UUID REFERENCES users(id),   -- nullable for system actions
    actor_type    TEXT NOT NULL,  -- 'user', 'device', 'system'
    action        TEXT NOT NULL,  -- 'device.claim', 'ota.release', 'user.login'
    resource_type TEXT NOT NULL,
    resource_id   TEXT,
    details       JSONB,
    ip_address    INET,
    user_agent    TEXT,
    created_at    TIMESTAMPTZ DEFAULT now()
);

CREATE INDEX idx_audit_org_time ON audit_log (org_id, created_at DESC);
CREATE INDEX idx_audit_actor ON audit_log (actor_id, created_at DESC);

-- ============================================================
-- Full-Text Search (PostgreSQL tsvector)
-- Generated columns auto-update on row changes; GIN indexes
-- make @@ plainto_tsquery() lookups fast.
-- ============================================================
ALTER TABLE devices ADD COLUMN search_vector tsvector
    GENERATED ALWAYS AS (
        to_tsvector('english',
            coalesce(device_name, '') || ' ' ||
            coalesce(device_key, '') || ' ' ||
            coalesce(device_type, '')
        )
    ) STORED;
CREATE INDEX idx_devices_search ON devices USING GIN (search_vector);

ALTER TABLE users ADD COLUMN search_vector tsvector
    GENERATED ALWAYS AS (
        to_tsvector('english',
            coalesce(email, '') || ' ' ||
            coalesce(display_name, '')
        )
    ) STORED;
CREATE INDEX idx_users_search ON users USING GIN (search_vector);

ALTER TABLE organizations ADD COLUMN search_vector tsvector
    GENERATED ALWAYS AS (
        to_tsvector('english',
            coalesce(name, '') || ' ' ||
            coalesce(slug, '') || ' ' ||
            coalesce(brand_name, '')
        )
    ) STORED;
CREATE INDEX idx_orgs_search ON organizations USING GIN (search_vector);

ALTER TABLE audit_log ADD COLUMN search_vector tsvector
    GENERATED ALWAYS AS (
        to_tsvector('english',
            coalesce(action, '') || ' ' ||
            coalesce(resource_type, '') || ' ' ||
            coalesce(resource_id, '') || ' ' ||
            coalesce(details::text, '')
        )
    ) STORED;
CREATE INDEX idx_audit_search ON audit_log USING GIN (search_vector);
```

### ClickHouse Schema

```sql
CREATE TABLE device_telemetry (
    device_id       String,
    device_type     String,
    ts              DateTime64(3),

    -- Common metadata (always present)
    rssi            Int8,
    uptime_ms       UInt32,

    -- Computed fields (enriched by Go API at ingest)
    pv_power        Float32,
    battery_power   Float32,
    inverter_power  Float32,
    dc_load_power   Float32,
    system_status   UInt8,        -- 0=unknown, 1=charging, 2=discharging, 3=balanced
    min_soc_pct     Float32,
    max_soc_pct     Float32,
    total_energy_wh Float32,

    -- Raw device-specific measurements
    fields          Map(String, Float64),

    ingested_at     DateTime DEFAULT now()
) ENGINE = MergeTree
  PARTITION BY toYYYYMM(ts)
  ORDER BY (device_type, device_id, ts)
  TTL ts + INTERVAL 90 DAY DELETE
  SETTINGS index_granularity = 8192;

-- Hourly aggregate (for dashboard history charts).
-- Aggregates ALL device types. The typed computed columns (pv_power, etc.)
-- are 0 for device types that don't produce them, so avg/max stay 0 —
-- harmless. Per-type dashboards filter by device_type at query time.
CREATE MATERIALIZED VIEW telemetry_hourly
  ENGINE = AggregatingMergeTree
  PARTITION BY toYYYYMM(hour)
  ORDER BY (device_type, device_id, hour)
AS SELECT
    device_type,
    device_id,
    toStartOfHour(ts) AS hour,
    avgState(pv_power)         AS pv_power_avg,
    maxState(pv_power)         AS pv_power_max,
    avgState(battery_power)    AS battery_power_avg,
    avgState(inverter_power)   AS inverter_power_avg,
    avgState(dc_load_power)    AS dc_load_power_avg,
    argMaxState(total_energy_wh, ts) AS energy_last,
    minState(min_soc_pct)      AS min_soc_pct,
    maxState(max_soc_pct)      AS max_soc_pct,
    countState()               AS sample_count
FROM device_telemetry
GROUP BY device_type, device_id, hour;
```

## API Design

All endpoints are versioned under `/api/v1`. The `v1` prefix lets the backend ship breaking changes under `v2` without bricking deployed ESP32s (which pin to `v1` and can't be upgraded until OTA). Device-facing endpoints (`/api/v1/ota/check`, `/api/v1/commands/{key}/pending`) must remain backward-compatible within `v1`.

### Standard Response Envelopes

**Success (single item):**
```json
{ "data": { ... } }
```

**Success (paginated list):**
```json
{
  "data": [ ... ],
  "pagination": {
    "total": 142,
    "limit": 20,
    "offset": 0,
    "has_more": true
  }
}
```

**Error:**
```json
{
  "error": {
    "code": "validation_error",
    "message": "email is required",
    "field": "email",
    "request_id": "req_abc123"
  }
}
```

Standard error codes and HTTP status:

| Code | HTTP | Meaning |
|---|---|---|
| `bad_request` | 400 | Malformed request body |
| `validation_error` | 400 | Failed field validation (`field` set) |
| `unauthorized` | 401 | Missing/invalid token |
| `forbidden` | 403 | Authenticated but not allowed |
| `not_found` | 404 | Resource doesn't exist |
| `conflict` | 409 | Duplicate / state conflict |
| `rate_limited` | 429 | Rate limit hit (see `Retry-After`) |
| `maintenance` | 503 | Platform in maintenance mode |
| `internal_error` | 500 | Unexpected server error |

Every response includes a `request_id` (UUID, also in `X-Request-Id` header) for log correlation.

### Pagination

List endpoints accept `?limit=20&offset=0` (max limit 100). Responses use the paginated envelope above. For high-volume tables (audit_log, telemetry history), cursor pagination is preferred — pass `?before=<iso8601_ts>` instead of offset to avoid scanning skipped rows.

### REST Endpoints

All paths below are relative to `/api/v1`.

```
# ── Auth ──────────────────────────────────────
POST   /api/auth/register          # Create account
POST   /api/auth/login             # Get JWT
POST   /api/auth/refresh           # Rotate tokens
POST   /api/auth/forgot-password   # Send reset email
POST   /api/auth/reset-password    # Complete reset

# ── OAuth ─────────────────────────────────────
GET    /api/auth/oauth/{provider}          # Redirect to provider (google, github)
GET    /api/auth/oauth/{provider}/callback # OAuth callback → JWT
POST   /api/auth/oauth/link               # Link OAuth account to existing user
DELETE /api/auth/oauth/{provider}          # Unlink OAuth account
GET    /api/auth/oauth/providers           # List configured OAuth providers

# ── Org SSO ────────────────────────────────────
POST   /api/orgs/{id}/sso                 # Configure OIDC provider
GET    /api/orgs/{id}/sso                 # Get SSO config
PATCH  /api/orgs/{id}/sso/{provider_id}   # Update SSO config
DELETE /api/orgs/{id}/sso/{provider_id}   # Remove SSO config
GET    /api/auth/sso/{org_slug}           # Redirect to org SSO

# ── Users ─────────────────────────────────────
GET    /api/users/me                # Current user profile
PATCH  /api/users/me                # Update profile
DELETE /api/users/me                # Delete account

# ── Organizations ─────────────────────────────
POST   /api/orgs                    # Create org
GET    /api/orgs                    # List user's orgs
GET    /api/orgs/{id}               # Org details
PATCH  /api/orgs/{id}               # Update org
POST   /api/orgs/{id}/members       # Invite member
DELETE /api/orgs/{id}/members/{uid} # Remove member

# ── Device Types ──────────────────────────────
POST   /api/orgs/{id}/device-types  # Register new type
GET    /api/orgs/{id}/device-types  # List types
PATCH  /api/orgs/{id}/device-types/{key}  # Update type

# ── Devices ───────────────────────────────────
GET    /api/devices                 # List user's devices
GET    /api/devices/{key}           # Device details
PATCH  /api/devices/{key}           # Update name, config
POST   /api/devices/{key}/claim    # Claim unowned device
DELETE /api/devices/{key}           # Remove device

# ── Telemetry ─────────────────────────────────
GET    /api/telemetry/{key}/latest  # Latest snapshot
GET    /api/telemetry/{key}/history # Time-range query
         ?range=24h&metric=power&bucket=5m

# ── Data Export (GDPR) ────────────────────────
POST   /api/export/request          # Request full data export (async)
         → returns job_id, emails when ready
GET    /api/export/status/{job_id}  # Check export job status
GET    /api/export/download/{job_id} # Download exported data (CSV/JSON zip)

# ── Commands ──────────────────────────────────
POST   /api/commands                # Send command to device
GET    /api/commands/{key}/pending  # Device polls this

# ── OTA ───────────────────────────────────────
POST   /api/ota/releases            # Create release
GET    /api/ota/releases            # List releases
PATCH  /api/ota/releases/{id}       # Update rollout %
POST   /api/ota/check/{key}         # Device polls for update
         ?current_ver=2.0.0
POST   /api/ota/report/{key}        # Device reports result

# ── Alerts ────────────────────────────────────
GET    /api/alerts/rules            # List alert rules
POST   /api/alerts/rules            # Create alert rule
PATCH  /api/alerts/rules/{id}       # Update rule
DELETE /api/alerts/rules/{id}       # Delete rule
GET    /api/alerts/events           # Query alert history
         ?org={id}&status=firing&since=7d
POST   /api/alerts/events/{id}/ack  # Acknowledge alert

# ── Email Templates ───────────────────────────
GET    /api/email/templates         # List templates
PATCH  /api/email/templates/{key}   # Update template
POST   /api/email/test/{key}        # Send test email

# ── Audit ────────────────────────────────────
GET    /api/audit                    # Query audit log
         ?org={id}&action=device.*&since=7d

# ── Billing / Invoices ─────────────────────────
GET    /api/billing/invoices         # List invoices (user sees own, admin sees all)
GET    /api/billing/invoices/{id}    # Invoice details
POST   /api/billing/invoices        # Create invoice (admin only)
PATCH  /api/billing/invoices/{id}    # Update invoice (admin only)
POST   /api/billing/invoices/{id}/mark-paid   # Mark as paid, auto-upgrade license
POST   /api/billing/invoices/{id}/cancel      # Cancel invoice
GET    /api/billing/payment-methods  # List payment methods
POST   /api/billing/payment-methods  # Add payment method (admin)
GET    /api/billing/plans            # List available plans with pricing

# ── Admin ─────────────────────────────────────
GET    /api/admin/users              # List all users
GET    /api/admin/orgs               # List all orgs
GET    /api/admin/stats              # System stats
GET    /api/admin/billing/summary    # Revenue, active subscriptions, overdue

# ── Maintenance ───────────────────────────────
GET    /api/admin/maintenance        # Get current maintenance status
POST   /api/admin/maintenance       # Enable/disable maintenance mode
         { "enabled": true, "message": "Upgrading database..." }

# ── Search ────────────────────────────────────
GET    /api/search                   # Full-text search across entities
         ?q=solar&type=devices,users,orgs,audit
         &org={id}&limit=20&offset=0

# ── Device Groups ─────────────────────────────
GET    /api/groups                   # List groups (scoped to user's org)
POST   /api/groups                   # Create group
PATCH  /api/groups/{id}              # Update group
DELETE /api/groups/{id}              # Delete group
POST   /api/groups/{id}/devices     # Add devices to group
DELETE /api/groups/{id}/devices/{key}  # Remove device from group

# ── Device Tags ───────────────────────────────
GET    /api/devices/{key}/tags       # List tags for a device
POST   /api/devices/{key}/tags      # Set a tag
DELETE /api/devices/{key}/tags/{tag_key}  # Remove a tag

# ── Notification Preferences ─────────────────
GET    /api/users/me/notifications   # Get current user's preferences
PATCH  /api/users/me/notifications   # Update preferences

# ── Health ────────────────────────────────────
GET    /api/health                   # Service status (always returns 200)
         Response: {
           "status": "ok",
           "uptime_seconds": 3600,
           "services": {
             "postgres":   { "status": "ok", "latency_ms": 2 },
             "clickhouse": { "status": "ok", "latency_ms": 3 },
             "mosquitto":  { "status": "ok", "connected": true },
             "minio":      { "status": "ok", "latency_ms": 5 }
           }
         }
```

### WebSocket Protocol

```
Connection: ws://api:8080/api/v1/ws?token={jwt}

Client → Server:
  {"type": "subscribe", "device_keys": ["AABB..."]}
  {"type": "unsubscribe", "device_keys": ["AABB..."]}
  {"type": "ping"}

Server → Client:
  {"type": "telemetry", "device_key": "...", "ts": "...", "fields": {...}}
  {"type": "device_status", "device_key": "...", "online": true}
  {"type": "ota_status", "device_key": "...", "status": "updating", "version": "2.1.0"}
  {"type": "pong"}
  {"type": "error", "message": "..."}
```

**Live stream handler (API side):** the API subscribes to `live/#` on Mosquitto once. Each message is dispatched to two consumers:
1. **WebSocket hub** — forwards to browser sessions subscribed to that `device_key`
2. **Alert engine** — evaluates the org's alert rules against the enriched payload

```go
// internal/websocket.go — runs in cmd/api
func (h *Hub) onLiveMessage(msg MQTTMessage) {
    deviceKey := strings.TrimPrefix(msg.Topic(), "live/")
    var enriched Enriched
    json.Unmarshal(msg.Payload(), &enriched)

    // 1. Push to subscribed browsers
    h.Broadcast(deviceKey, enriched)

    // 2. Evaluate alerts (async so a slow rule doesn't delay WS push)
    go h.alerts.Evaluate(context.Background(), h.deviceByKey(deviceKey), &enriched)
}
```

The WebSocket push happens synchronously (sub-ms, in-memory); alert eval runs in a goroutine so a bad rule can't delay the live push to browsers.

### MQTT Topics

```
# Device → Platform (ESP32 publishes)
telemetry/{device_type}/{device_key}     # JSON payload (5s interval)
status/{device_key}/online               # LWT + birth message
status/{device_key}/ota                   # OTA result

# Platform → Device (device subscribes)
commands/{device_key}                     # Pending commands
ota/{device_key}                          # OTA trigger (push path)

# Platform internal
telemetry/#                               # ingest worker subscribes (all telemetry)
status/+/online                           # ingest worker subscribes (online/offline)
live/{device_key}                         # ingest publishes enriched data; API subscribes live/#
```

**Topic ownership by service:**
- `telemetry/#`, `status/+/online` → **ingest worker** subscribes
- `live/{device_key}` → **ingest worker** publishes, **API server** subscribes (`live/#`)
- `commands/{device_key}`, `ota/{device_key}` → API writes to PG; device polls API REST (MQTT push is a future optimization)

### MQTT Payload Format (Firmware ↔ Backend Contract)

This is the exact JSON the ESP32 publishes to `telemetry/{device_type}/{device_key}`. All device types share this envelope; the `data` object varies by device type. The Go API flattens `data` into the ClickHouse `fields` Map and extracts computed fields into typed columns.

```json
{
  "ts": 1720000000,
  "ts_ms": 500,
  "schema": "telemetry_v1",
  "fw": "2.0.0",
  "uptime_ms": 3600000,
  "rssi": -55,
  "heap_free": 150000,
  "data": {
    "ch0_V": 13.20, "ch0_I": 1.50, "ch0_P": 19.80, "energy_wh0": 120.5, "soc_pct0": 85.0,
    "ch1_V": 12.80, "ch1_I": -0.50, "ch1_P": -6.40, "energy_wh1": 95.2, "soc_pct1": 72.0,
    "ch2_V": 0.00, "ch2_I": 0.00, "ch2_P": 0.00, "energy_wh2": 0.0, "soc_pct2": 0.0,
    "ch3_V": 24.10, "ch3_I": 2.30, "ch3_P": 55.43, "energy_wh3": 410.0, "soc_pct3": 91.0,
    "coulomb_mah0": 8500, "coulomb_mah1": 7200,
    "ina3221_v0": 13.20, "ina3221_i0": 1.50,
    "ina226_v": 24.10, "ina226_i": 2.30, "ina226_p": 55.43,
    "ads1115_0": 3.30, "ads1115_1": 0.0, "ads1115_2": 0.0, "ads1115_3": 0.0,
    "relay0": 1, "relay1": 0, "relay2": 1, "relay3": 0
  }
}
```

**Envelope fields (present for every device type):**

| Field | Type | Notes |
|---|---|---|
| `ts` | int | Unix epoch seconds |
| `ts_ms` | int | Milliseconds within the second (0-999) |
| `schema` | string | Payload schema version, mirrors firmware `TELEMETRY_SCHEMA_VERSION` |
| `fw` | string | Firmware version (semver) |
| `uptime_ms` | int | Device uptime |
| `rssi` | int | WiFi signal strength, dBm |
| `heap_free` | int | Free heap, bytes |
| `data` | object | Type-specific flat key-value map. All values are numbers. |

**Rules:**
- `data` is a **flat** object — no nesting. Keys are device-type-specific (e.g., `ch0_V`, `temp_C`, `humidity`).
- All `data` values are JSON numbers (int or float). No strings, no booleans — relays use `1`/`0`.
- Unknown keys are ignored by the Go API (forward-compatible — new sensors don't break old parsers).
- Missing keys default to 0 in the ClickHouse Map.
- The Go API computes `pv_power`, `battery_power`, `inverter_power`, `system_status`, `min/max_soc_pct`, `total_energy_wh` from `data` + the device's `channel_groups` config, then writes them as typed columns. The raw `data` map goes into `fields`.

**Temperature sensor example (different device type, same envelope):**
```json
{
  "ts": 1720000000, "ts_ms": 0, "schema": "telemetry_v1", "fw": "1.0.0",
  "uptime_ms": 7200000, "rssi": -62, "heap_free": 95000,
  "data": { "temp_C": 23.4, "humidity": 61.0, "pressure_hPa": 1013.0, "battery_V": 3.2 }
}
```

For this device type, the computed columns (`pv_power`, etc.) are all 0 — the Go API only computes them for `power_monitor_v2`. The `fields` map holds `temp_C`, `humidity`, etc.

## Go Project Structure

One Go module, two binaries, shared `internal/` packages. Both binaries import the same `internal/` code — only `cmd/` differs.

```
backend/
├── cmd/
│   ├── api/
│   │   └── main.go           # HTTP server + WebSocket + Mosquitto auth backend
│   └── ingest/
│       └── main.go           # MQTT consumer + pipeline + alerts + email
├── internal/
│   ├── config.go             # Env → Config struct (shared)
│   ├── database.go           # PG + CH connection pools (shared)
│   ├── model.go              # All shared types (shared)
│   ├── auth.go               # JWT, password hashing, OAuth (shared; used by api)
│   ├── middleware.go         # Auth, logging, CORS, rate limit, maintenance (api)
│   ├── handlers.go           # REST endpoints (api)
│   ├── websocket.go          # WebSocket hub + live/# subscriber (api)
│   ├── alerts.go             # Alert rule evaluation on live/# stream (api)
│   ├── email.go              # Email queue + template rendering + SMTP (api)
│   ├── ota.go                # OTA release management + check endpoint (api)
│   ├── billing.go            # Invoice + license logic (api)
│   ├── search.go             # Full-text search handlers (api)
│   ├── ingest.go             # MQTT consumer pipeline (ingest)
│   ├── enricher.go           # Channel classification (ingest)
│   ├── store.go              # ClickHouse batch writer + retention (ingest)
│   ├── audit.go              # Audit log writer (shared)
│   ├── mqttauth.go           # Mosquitto HTTP auth backend endpoint (api)
│   └── fakes/
│       ├── clock.go          # FixedClock — deterministic time for tests
│       ├── idgen.go          # SequentialIDGen — predictable UUIDs
│       ├── mqtt.go           # FakePublisher — captures published messages
│       ├── resolver.go       # StubResolver — returns canned devices
│       ├── store.go          # MemStore — in-memory telemetry + email capture
│       ├── email.go          # FakeSender — records sent emails
│       └── builders.go       # Test data builders: aDevice(), anAlertRule()...
├── migrations/
│   ├── 001_initial.up.sql
│   └── 001_initial.down.sql
├── .env.example
├── Dockerfile                # Multi-stage — builds both binaries
├── Dockerfile.api            # (optional) per-binary images
├── Dockerfile.ingest
├── Makefile
├── go.mod
└── README.md
```

**Shared packages** (`config`, `database`, `model`, `audit`) are imported by both binaries. Each binary only wires the packages it needs — `cmd/api/main.go` constructs the HTTP server + WebSocket hub + Mosquitto auth handler; `cmd/ingest/main.go` constructs the MQTT subscriber + pipeline + batch writer. Both connect to the same PG and ClickHouse pools.

**Docker:** one multi-stage `Dockerfile` builds both binaries into a slim image, or two per-binary images (`Dockerfile.api`, `Dockerfile.ingest`) for smaller deploys. Docker Compose runs them as separate services.

## Testing Strategy

Testability is a design constraint, not an afterthought. Two rules make the codebase testable:

1. **Every external dependency is a small interface.** Databases, MQTT, SMTP, the clock, ID generation — all injected, all fakeable. No package calls `time.Now()` or `uuid.New()` directly.
2. **Pure logic has no dependencies.** The enricher, alert condition evaluator, quiet-hours check, and payload parser are pure functions: input → output, no I/O. These get exhaustive unit tests; the wiring around them gets integration tests.

### Injectable Boundaries

```go
// internal/model.go — interfaces at the seams. Real implementations live in
// database.go / email.go / ingest.go; tests pass fakes from internal/fakes.

// Clock replaces time.Now() so time-based logic (quiet hours, retention
// cutoffs, alert duration counters, JWT expiry) is deterministic in tests.
type Clock interface {
    Now() time.Time
}
type realClock struct{}
func (realClock) Now() time.Time { return time.Now() }

// IDGenerator replaces uuid.New() so tests get predictable IDs.
type IDGenerator interface {
    New() string
}

// MQTTPublisher is the republish seam. The real one wraps paho; the fake
// captures published messages for assertions.
type MQTTPublisher interface {
    Publish(topic string, qos byte, retained bool, payload []byte) error
}

// DeviceResolver is the device-lookup seam. Real one hits PG with a cache;
// the fake returns a canned Device.
type DeviceResolver interface {
    Resolve(ctx context.Context, deviceKey string) (*Device, error)
}

// TelemetryStore is the ClickHouse write seam.
type TelemetryStore interface {
    Write(ctx context.Context, row TelemetryRow) error
    Flush(ctx context.Context) error
}

// EmailSender is the SMTP seam. The fake records messages in memory; tests
// never hit a real SMTP server.
type EmailSender interface {
    Send(ctx context.Context, msg EmailMessage) error
}
```

Every struct (`Pipeline`, `AlertEngine`, `EmailService`, handlers) takes these as constructor args. `cmd/*/main.go` wires the real implementations; tests wire fakes.

### Test Layers

| Layer | What | Tool | Speed |
|---|---|---|---|
| **Unit** | Pure logic: enricher, alert condition eval, quiet hours, payload parse, channel classification, validation | stdlib `testing` | ms |
| **Handler** | HTTP handlers with fake services, `httptest.NewRecorder`, chi router | `net/http/httptest` | ms |
| **Pipeline** | `Pipeline.Process()` with fake resolver/store/publisher — no real MQTT or DB | fakes | ms |
| **Integration** | Real PG + ClickHouse via testcontainers; runs migrations, tests SQL + batch writer + retention | `testcontainers-go` | seconds |
| **MQTT** | Real Mosquitto via testcontainers; end-to-end publish → ingest → store | `testcontainers-go` | seconds |
| **Email** | `EmailService.DrainLoop` with fake `EmailSender`; asserts queue state + retry/backoff | fakes | ms |

### Fake Package

```
internal/
├── fakes/
│   ├── clock.go           # FixedClock — returns a set time
│   ├── idgen.go           # SequentialIDGen — predictable UUIDs
│   ├── mqtt.go            # FakePublisher — captures published messages
│   ├── resolver.go        # StubResolver — returns canned devices
│   ├── store.go           # MemStore — in-memory telemetry rows
│   ├── email.go           # FakeSender — records sent emails
│   └── builders.go        # Test data builders: aDevice(), anAlertRule(), anEnriched()
```

`builders.go` gives fluent constructors so tests read like specs:
```go
dev := aDevice("AABBCCDDEEFF").withType("power_monitor_v2").ownedBy(user).build()
rule := anAlertRule("High PV").field("pv_power").gt(5000).forDuration(30 * time.Second).build()
```

### Example: Pipeline unit test (no DB, no MQTT)

```go
func TestPipeline_Process_RepublishesEnrichedToLive(t *testing.T) {
    clock := fakes.FixedClock(at("2026-07-12T10:00:00Z"))
    pub := &fakes.FakePublisher{}
    pipe := ingest.NewPipeline(
        &fakes.StubResolver{Device: aDevice("AABBCCDDEEFF").build()},
        ingest.NewValidator(),
        ingest.NewEnricher(),
        ingest.NewLicenseChecker(fakes.StubLicensor{Allow: true}),
        &fakes.MemStore{},
        pub,                       // MQTTPublisher fake
        audit.New(audit.NewMemSink()),
        clock,
    )

    msg := fakes.aMQTTMessage("telemetry/power_monitor_v2/AABBCCDDEEFF", samplePayload())
    err := pipe.Process(context.Background(), msg)
    require.NoError(t, err)

    require.Len(t, pub.Messages, 1)
    assert.Equal(t, "live/AABBCCDDEEFF", pub.Messages[0].Topic)
    // assert enriched fields present in the republished payload
}
```

### Example: Enricher pure-function test

```go
func TestEnricher_ClassifiesChannelsByGroup(t *testing.T) {
    e := ingest.NewEnricher()
    groups := []ChannelGroup{{Icon: 0, ChannelMask: 0b0001}} // ch0 = solar
    raw := payload{"ch0_P": 19.8, "ch1_P": -6.4}

    got := e.Enrich(aDevice("X").withGroups(groups).build(), raw)

    assert.Equal(t, 19.8, got.PvPower)        // ch0 classified as PV
    assert.Equal(t, -6.4, got.BatteryPower)    // ch1 (unclassified) falls to battery fallback
    assert.Equal(t, SystemCharging, got.SystemStatus)
}
```

### Example: Quiet-hours test (the clock matters)

```go
func TestShouldSend_QuietHoursWrapMidnight(t *testing.T) {
    svc := email.New(pgFake, fakes.FixedClock(at("2026-07-12T23:30:00Z")))
    setUserPrefs(t, svc, user, quietHours(22, 7)) // 10pm-7am

    assert.False(t, svc.ShouldSend(ctx, user, "alert_fired")) // 23:30 is in quiet hours
}
```

Without the injectable clock, this test would be flaky (depends on when it runs). With it, it's deterministic.

### Integration tests with testcontainers

```go
//go:build integration

func TestBatchWriter_WritesToClickHouse(t *testing.T) {
    ctx := context.Background()
    chContainer, _ := testcontainers.Run(ctx, "clickhouse/clickhouse-server:24.3-alpine")
    defer chContainer.Terminate(ctx)
    chConn := connectClickHouse(t, chContainer)
    applyClickHouseSchema(t, chConn)

    bw := ingest.NewBatchWriter(chConn, pgFake, fakes.RealClock())
    for i := 0; i < 5; i++ {
        bw.Write(ctx, aTelemetryRow("AABBCCDDEEFF", sample))
    }
    bw.Flush(ctx)

    var count uint64
    chConn.QueryRow(ctx, "SELECT count() FROM device_telemetry").Scan(&count)
    assert.Equal(t, uint64(5), count)
}
```

PostgreSQL integration tests follow the same pattern — `testcontainers.Run` a `postgres:16-alpine`, run migrations via `golang-migrate`, test queries. Build tag `//go:build integration` keeps them out of the fast unit-test run.

### Makefile targets

```makefile
test:                 ## fast unit + handler + pipeline tests (no Docker)
	go test ./...

test-integration:    ## spins up PG + CH + Mosquitto via testcontainers
	go test -tags integration ./...

test-cover:          ## coverage report for internal/
	go test -coverprofile=cover.out ./internal/...
	go tool cover -html=cover.out

bench:                ## enricher / alert eval hot paths
	go test -bench=. ./internal/ingest
```

CI runs `make test` on every push (fast, <10s); `make test-integration` on PRs (spins containers, ~60s).

### Coverage expectations

| Package | Target | Why |
|---|---|---|
| `internal/enricher` | 95% | pure logic, security-relevant (classification drives alerts) |
| `internal/alerts` (condition eval) | 95% | pure logic |
| `internal/ingest` (pipeline) | 85% | wiring, covered by unit + integration |
| `internal/email` (queue + quiet hours) | 90% | time-sensitive, retry logic |
| `internal/handlers` | 80% | HTTP wiring, validated via handler tests |
| `internal/database` | 70% | thin wrappers, real coverage from integration tests |

Coverage is a floor, not a goal — a missing branch on the enricher matters more than 100% on a getter.

## Ingestion Pipeline (Go)

```go
// internal/ingest.go — MQTT → Parse → Validate → Enrich → Store

type Pipeline struct {
    resolver DeviceResolver     // cache + PG lookup (interface — fakeable in tests)
    validator *Validator         // schema-based validation (pure logic)
    enricher  *Enricher          // channel classification, system status (pure logic)
    licensor  *LicenseChecker    // plan enforcement
    store     TelemetryStore     // ClickHouse batch writer (interface — fakeable)
    mqtt      MQTTPublisher      // republish enriched data to live/{key}
    auditor   *Auditor           // audit log writer
    clock     Clock              // injectable time — see Testing Strategy
}

// Process runs in the ingest worker. It is the hot path — keep it fast and
// never block on business logic. Alerts and email live in the API, which
// consumes the live/{device_key} stream this republishes.
func (p *Pipeline) Process(ctx context.Context, msg MQTTMessage) error {
    raw, err := parseJSON(msg.Payload)
    if err != nil {
        return fmt.Errorf("parse: %w", err)
    }

    device, err := p.resolver.Resolve(ctx, msg.DeviceKey())
    if err != nil {
        return fmt.Errorf("resolve device %s: %w", msg.DeviceKey(), err)
    }

    if err := p.validator.Validate(device.Type, raw); err != nil {
        p.auditor.Log(ctx, AuditEntry{
            Action: "telemetry.validation_failed",
            Detail: err.Error(),
        })
        return fmt.Errorf("validate: %w", err)
    }

    enriched := p.enricher.Enrich(device, raw)

    if err := p.licensor.CheckIngest(device); err != nil {
        p.auditor.Log(ctx, AuditEntry{
            Action: "telemetry.license_blocked",
            Detail: err.Error(),
        })
        return fmt.Errorf("license: %w", err)
    }

    p.store.Write(ctx, TelemetryRow{
        DeviceID:    device.ID,
        DeviceType:  device.Type,
        Timestamp:   enriched.Ts,
        Fields:      enriched.Fields,
        Computed:    enriched.Computed,
    })

    // Republish enriched reading to live/{device_key} (retained=false, QoS 0).
    // The API server subscribes to live/# and uses each message for BOTH
    // WebSocket fan-out AND alert rule evaluation.
    // QoS 0: live data is lossy by design — a dropped frame is fine, the next
    // one arrives in 5s. Do not block ingestion on the republish.
    p.mqtt.Publish("live/"+device.Key, 0, false, enriched.ToWSJSON())

    if enriched.StatusChanged {
        p.auditor.Log(ctx, AuditEntry{
            OrgID:        device.OrgID,
            ActorID:      device.ID,
            ActorType:    "device",
            Action:       "device.status_change",
            ResourceType: "device",
            ResourceID:   device.Key,
            Details:      map[string]any{"status": enriched.SystemStatus},
        })
    }

    return nil
}
```

## Batching Strategy

```go
// internal/store.go — ClickHouse batch writer

type BatchWriter struct {
    buf    chan TelemetryRow    // buffered channel, cap 10000
    chPool *clickhouse.Conn
    pgPool *pgxpool.Pool
}

func (bw *BatchWriter) Write(ctx context.Context, row TelemetryRow) {
    select {
    case bw.buf <- row:
    default:
        log.Warn("ingest buffer full, dropping row")
    }
}

func (bw *BatchWriter) FlushLoop(ctx context.Context) {
    batch := make([]TelemetryRow, 0, 1000)
    ticker := time.NewTicker(30 * time.Second)
    defer ticker.Stop()

    for {
        select {
        case <-ticker.C:
            if len(batch) > 0 {
                bw.flush(ctx, batch)
                batch = batch[:0]
            }
        case row := <-bw.buf:
            batch = append(batch, row)
            if len(batch) >= 1000 {
                bw.flush(ctx, batch)
                batch = batch[:0]
            }
        case <-ctx.Done():
            if len(batch) > 0 {
                bw.flush(ctx, batch)
            }
            return
        }
    }
}
```

## Alert Engine

```go
// internal/alerts.go — runs in the API server, fed by the live/# MQTT stream.
// Not in the ingest worker: a bad rule must never endanger telemetry storage.

type AlertEngine struct {
    pg       *pgxpool.Pool
    email    *EmailQueue       // enqueues, does not send synchronously
    auditor  *Auditor
    mu       sync.RWMutex
    // In-memory state: tracks consecutive matches per (rule_id, device_key)
    counters map[string]int  // key = "rule_id:device_key"
}

type AlertRule struct {
    ID           string
    OrgID        string
    DeviceType   string    // empty = all types
    DeviceID     string    // empty = all devices in org
    Field        string    // payload field to evaluate
    Operator     string    // gt, lt, gte, lte, eq, neq
    Value        float64
    DurationSec  int       // 0 = fire immediately
    NotifyEmail  bool
}

// Evaluate is called after each telemetry enrichment.
func (e *AlertEngine) Evaluate(ctx context.Context, device *Device, enriched *Enriched) error {
    rules, err := e.loadRules(ctx, device.OrgID)
    if err != nil {
        return fmt.Errorf("load rules: %w", err)
    }

    for _, rule := range rules {
        if !e.matchesDevice(rule, device) {
            continue
        }

        rawValue := enriched.GetField(rule.Field)
        matched := e.evaluateCondition(rawValue, rule.Operator, rule.Value)

        key := rule.ID + ":" + device.Key
        e.mu.Lock()
        if matched {
            e.counters[key]++ // track consecutive matches
        } else {
            delete(e.counters, key) // reset streak on non-match
        }
        count := e.counters[key]
        e.mu.Unlock()

        if matched && count >= requiredSamples(rule.DurationSec) {
            e.fire(ctx, rule, device, rawValue)
        } else if !matched {
            e.resolve(ctx, rule, device, rawValue)
        }
    }
    return nil
}

// requiredSamples converts a duration in seconds to a consecutive-sample count.
// Telemetry arrives every 5s, so samples = ceil(duration_sec / 5).
// duration_sec=0 (immediate) requires exactly 1 matching sample.
func requiredSamples(durationSec int) int {
    if durationSec <= 0 {
        return 1
    }
    return (durationSec + 4) / 5 // ceil division by 5s interval
}

func (e *AlertEngine) fire(ctx context.Context, rule AlertRule, device *Device, value float64) {
    // Check if already firing (avoid duplicate events)
    var existingID string
    err := e.pg.QueryRow(ctx,
        `SELECT id FROM alert_events
         WHERE rule_id = $1 AND device_key = $2 AND status = 'firing'
         LIMIT 1`, rule.ID, device.Key).Scan(&existingID)
    if err == nil {
        return // already firing
    }

    // Create alert event
    var eventID string
    e.pg.QueryRow(ctx,
        `INSERT INTO alert_events (rule_id, device_key, status, fired_value)
         VALUES ($1, $2, 'firing', $3) RETURNING id`,
        rule.ID, device.Key, value).Scan(&eventID)

    // Enqueue emails (async — never block alert eval on SMTP)
    if rule.NotifyEmail {
        e.email.EnqueueAlert(ctx, "alert_fired", device, rule, value)
    }

    e.auditor.Log(ctx, AuditEntry{
        OrgID:        rule.OrgID,
        ActorID:      device.ID,
        ActorType:    "system",
        Action:       "alert.fired",
        ResourceType: "alert_rule",
        ResourceID:   rule.ID,
        Details:      map[string]any{"device_key": device.Key, "value": value},
    })
}

func (e *AlertEngine) resolve(ctx context.Context, rule AlertRule, device *Device, value float64) {
    // Find and resolve firing event
    tag, err := e.pg.Exec(ctx,
        `UPDATE alert_events
         SET status = 'resolved', resolved_at = now(), resolved_value = $3
         WHERE rule_id = $1 AND device_key = $2 AND status = 'firing'`,
        rule.ID, device.Key, value)
    if err != nil || tag.RowsAffected() == 0 {
        return // wasn't firing
    }

    if rule.NotifyEmail {
        e.email.EnqueueAlert(ctx, "alert_resolved", device, rule, value)
    }

    e.auditor.Log(ctx, AuditEntry{
        OrgID:        rule.OrgID,
        ActorID:      device.ID,
        ActorType:    "system",
        Action:       "alert.resolved",
        ResourceType: "alert_rule",
        ResourceID:   rule.ID,
        Details:      map[string]any{"device_key": device.Key, "value": value},
    })
}
```

## Email Service

Email is fully async. Triggering code (auth handlers, alert engine, billing) enqueues a row into `email_queue`; a background worker in the API drains the queue, renders the template, applies notification preferences + quiet hours, and sends via SMTP with retry. SMTP latency never blocks a request or the alert eval loop.

```go
// internal/email.go — runs in the API server

type EmailService struct {
    pg       *pgxpool.Pool
    fromAddr string
    smtpHost string
    smtpPort int
    smtpUser string
    smtpPass string
    platform string
    baseURL  string
}

// EnqueueAlert is called by the alert engine. It checks each org member's
// notification preferences + quiet hours, then inserts one email_queue row
// per qualifying recipient. Returns immediately — sending happens async.
func (e *EmailService) EnqueueAlert(ctx context.Context, templateKey string, device *Device, rule AlertRule, value float64) {
    data := map[string]any{
        "RuleName":     rule.Name,
        "DeviceName":   device.Name,
        "DeviceKey":    device.Key,
        "Value":        value,
        "Threshold":    rule.Value,
        "Unit":         "",
        "DashboardURL": e.baseURL + "/devices/" + device.Key,
    }
    candidates, err := e.getOrgNotifyEmails(ctx, device.OrgID, templateKey+"_email")
    if err != nil {
        return
    }
    for _, r := range candidates {
        if !e.shouldSend(ctx, r.UserID, templateKey) {
            continue // user disabled this type, or in quiet hours
        }
        e.pg.Exec(ctx,
            `INSERT INTO email_queue (template_key, recipient, user_id, data)
             VALUES ($1, $2, $3, $4)`,
            templateKey, r.Email, r.UserID, data)
    }
}

// Enqueue is the generic enqueue path for non-alert emails (welcome,
// password_reset, payment_confirmed). Called by auth/billing handlers.
func (e *EmailService) Enqueue(ctx context.Context, templateKey, recipient string, userID string, data map[string]any) {
    e.pg.Exec(ctx,
        `INSERT INTO email_queue (template_key, recipient, user_id, data)
         VALUES ($1, $2, $3, $4)`,
        templateKey, recipient, userID, data)
}

// DrainLoop runs as a background goroutine in the API. Polls email_queue,
// claims a batch, renders, sends, updates status. One row at a time is fine
// for v1 volume; batch sending is a later optimization.
func (e *EmailService) DrainLoop(ctx context.Context) {
    ticker := time.NewTicker(5 * time.Second)
    defer ticker.Stop()
    for {
        select {
        case <-ticker.C:
            e.drainBatch(ctx)
        case <-ctx.Done():
            return
        }
    }
}

func (e *EmailService) drainBatch(ctx context.Context) {
    // Claim up to 20 due rows (FOR UPDATE SKIP LOCKED so multiple API
    // instances don't double-send).
    rows, err := e.pg.Query(ctx, `
        SELECT id, template_key, recipient, data FROM email_queue
        WHERE status = 'queued' AND next_attempt_at <= now()
        ORDER BY next_attempt_at LIMIT 20
        FOR UPDATE SKIP LOCKED`)
    if err != nil {
        return
    }
    defer rows.Close()
    for rows.Next() {
        var id int64
        var key, recipient string
        var data []byte
        rows.Scan(&id, &key, &recipient, &data)
        e.sendOne(ctx, id, key, recipient, data)
    }
}

func (e *EmailService) sendOne(ctx context.Context, id int64, key, recipient string, data []byte) {
    // Mark sending
    e.pg.Exec(ctx, `UPDATE email_queue SET status='sending', attempts=attempts+1 WHERE id=$1`, id)

    tmpl, err := e.loadTemplate(ctx, key)
    if err != nil {
        e.failOne(ctx, id, err)
        return
    }
    var vars map[string]any
    json.Unmarshal(data, &vars)
    subject, _ := e.renderTemplate(tmpl.Subject, vars)
    bodyHTML, _ := e.renderTemplate(tmpl.BodyHTML, vars)
    bodyText, _ := e.renderTemplate(tmpl.BodyText, vars)

    msg := gomail.NewMessage()
    msg.SetHeader("From", e.fromAddr)
    msg.SetHeader("To", recipient)
    msg.SetHeader("Subject", subject)
    msg.SetBody("text/plain", bodyText)
    msg.AddAlternative("text/html", bodyHTML)

    dialer := gomail.NewDialer(e.smtpHost, e.smtpPort, e.smtpUser, e.smtpPass)
    if err := dialer.DialAndSend(msg); err != nil {
        e.failOne(ctx, id, err) // reschedules with backoff, or marks failed after 3
        return
    }
    e.pg.Exec(ctx, `UPDATE email_queue SET status='sent', sent_at=now() WHERE id=$1`, id)
}

func (e *EmailService) failOne(ctx context.Context, id int64, err error) {
    // Exponential backoff: 1m, 5m, 25m, then give up.
    e.pg.Exec(ctx, `
        UPDATE email_queue
        SET status = CASE WHEN attempts >= 3 THEN 'failed' ELSE 'queued' END,
            last_error = $2,
            next_attempt_at = now() + (interval '1 minute' * (5 ^ attempts))
        WHERE id = $1`, id, err.Error())
    log.Error("email send failed", "id", id, "error", err)
}

// shouldSend checks a user's notification preferences + quiet hours.
func (e *EmailService) shouldSend(ctx context.Context, userID string, alertType string) bool {
    var prefs struct {
        AlertFired    bool
        AlertResolved bool
        QuietStart    *int
        QuietEnd      *int
    }
    err := e.pg.QueryRow(ctx,
        `SELECT alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end
         FROM notification_preferences WHERE user_id = $1`, userID).Scan(
        &prefs.AlertFired, &prefs.AlertResolved, &prefs.QuietStart, &prefs.QuietEnd)
    if err != nil {
        return true // default to send on error
    }
    switch alertType {
    case "alert_fired":
        if !prefs.AlertFired {
            return false
        }
    case "alert_resolved":
        if !prefs.AlertResolved {
            return false
        }
    }
    if prefs.QuietStart != nil && prefs.QuietEnd != nil {
        hour := time.Now().UTC().Hour()
        if *prefs.QuietStart < *prefs.QuietEnd {
            if hour >= *prefs.QuietStart && hour < *prefs.QuietEnd {
                return false
            }
        } else {
            if hour >= *prefs.QuietStart || hour < *prefs.QuietEnd {
                return false
            }
        }
    }
    return true
}
```

The `FOR UPDATE SKIP LOCKED` claim lets multiple API instances drain the queue concurrently without double-sending — a natural fit if the API is later scaled horizontally behind Caddy.

## Maintenance Mode

```go
// internal/middleware.go — Maintenance mode check

func MaintenanceMiddleware(maintenance *MaintenanceMode) func(http.Handler) http.Handler {
    return func(next http.Handler) http.Handler {
        return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            // Always allow health checks (unversioned, for load balancer probes)
            if r.URL.Path == "/health" || r.URL.Path == "/api/v1/health" {
                next.ServeHTTP(w, r)
                return
            }

            if maintenance.IsEnabled() {
                writeJSON(w, 503, map[string]string{
                    "error":   "maintenance",
                    "message": maintenance.Message(),
                })
                return
            }

            next.ServeHTTP(w, r)
        })
    }
}

// internal/ingest.go — MQTT consumer checks maintenance flag
func (p *Pipeline) Consume(ctx context.Context) {
    messages := make(chan MQTTMessage, 100)
    p.mqtt.Subscribe("telemetry/#", 0, func(_ *mqtt.Client, msg mqtt.Message) {
        if p.maintenance.IsEnabled() {
            msg.Ack()  // acknowledge but don't process
            return
        }
        messages <- msg
    })
    // ... worker pool processes messages
}
```

## OAuth / SSO

```go
// internal/auth.go — OAuth handlers

type OAuthProvider interface {
    AuthURL(state string) string
    Exchange(ctx context.Context, code string) (*OAuthUser, error)
    Name() string
}

type OAuthUser struct {
    Provider    string
    ProviderID  string
    Email       string
    DisplayName string
    AvatarURL   string
}

// Built-in providers
var Providers = map[string]OAuthProvider{
    "google": NewGoogleProvider(...),
    "github": NewGitHubProvider(...),
}

// OAuth callback handler
func (h *Handlers) OAuthCallback(w http.ResponseWriter, r *http.Request) {
    provider := chi.URLParam(r, "provider")
    code := r.URL.Query().Get("code")
    state := r.URL.Query().Get("state")

    // Validate state (CSRF protection)
    if state != r.Context().Value("oauth_state") {
        http.Error(w, "invalid state", 403)
        return
    }

    // Exchange code for user info
    p := Providers[provider]
    oauthUser, err := p.Exchange(r.Context(), code)
    if err != nil {
        http.Error(w, "oauth exchange failed", 502)
        return
    }

    // Find or create user
    user, err := h.findOrCreateUser(r.Context(), oauthUser)
    if err != nil {
        http.Error(w, "user creation failed", 500)
        return
    }

    // Issue JWT
    token, err := h.jwt.Issue(user.ID)
    writeJSON(w, 200, map[string]string{"token": token, "redirect": "/dashboard"})
}

// Org SSO: validate ID token against configured OIDC provider
func (h *Handlers) SSOCallback(w http.ResponseWriter, r *http.Request) {
    orgSlug := chi.URLParam(r, "org_slug")
    idToken := r.URL.Query().Get("id_token")

    // Look up org's OIDC provider
    provider, err := h.getOrgProvider(r.Context(), orgSlug)
    if err != nil {
        http.Error(w, "org not configured for SSO", 404)
        return
    }

    // Verify ID token
    verifier := oidc.NewVerifier(provider.IssuerURL, h.oidcKeySet, &oidc.Config{
        ClientID: provider.ClientID,
    })
    idTokenParsed, err := verifier.Verify(r.Context(), idToken)
    if err != nil {
        http.Error(w, "invalid token", 403)
        return
    }

    // Extract claims
    var claims struct { Email string `json:"email"` }
    idTokenParsed.Claims(&claims)

    // Check email domain matches org
    if !h.emailDomainMatchesOrg(claims.Email, orgSlug) {
        http.Error(w, "email domain not authorized for this org", 403)
        return
    }

    // Find or create user, add to org
    user, _ := h.findOrCreateUserByEmail(r.Context(), claims.Email)
    h.ensureOrgMembership(r.Context(), user.ID, provider.OrgID)

    token, _ := h.jwt.Issue(user.ID)
    writeJSON(w, 200, map[string]string{"token": token, "redirect": "/dashboard"})
}
```

## Rate Limiting

```go
// internal/middleware.go — Token bucket rate limiter

type RateLimiter struct {
    store     *redis.Client  // or in-memory map for v1
    requests map[string]*tokenBucket
    mu        sync.Mutex
}

type tokenBucket struct {
    tokens    float64
    lastRefill time.Time
    maxTokens float64
    refillRate float64  // tokens per second
}

// Per-IP limiter for auth endpoints: 10 requests/minute
func (h *Handlers) RateLimitAuth(next http.Handler) http.Handler {
    limiter := NewRateLimiter(10, 10)  // max 10, refill 10 per 60s
    return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        key := "auth:" + r.RemoteAddr
        if !limiter.Allow(key) {
            w.Header().Set("Retry-After", "60")
            http.Error(w, `{"error":"rate_limit","message":"too many requests"}`, 429)
            return
        }
        next.ServeHTTP(w, r)
    })
}

// Per-token limiter for API endpoints: configurable per plan
func (h *Handlers) RateLimitAPI(next http.Handler) http.Handler {
    return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        user := r.Context().Value("user").(*User)
        limit := h.getPlanRateLimit(user.PlanID)  // from license_plans
        key := "api:" + user.ID
        remaining, reset := h.slidingWindow(key, limit)
        w.Header().Set("X-RateLimit-Limit", strconv.Itoa(limit))
        w.Header().Set("X-RateLimit-Remaining", strconv.Itoa(remaining))
        w.Header().Set("X-RateLimit-Reset", strconv.Itoa(reset))
        if remaining == 0 {
            http.Error(w, `{"error":"rate_limit"}`, 429)
            return
        }
        next.ServeHTTP(w, r)
    })
}

// Rate limits per plan (configurable via license_plans table):
//   Free:      60 req/min
//   Pro:      300 req/min
//   Business: 1000 req/min
//   Enterprise: 5000 req/min
```

## Search

```go
// internal/handlers.go — Full-text search across entities

func (h *Handlers) Search(w http.ResponseWriter, r *http.Request) {
    q := r.URL.Query().Get("q")
    types := r.URL.Query().Get("type")  // comma-separated: devices,users,orgs,audit
    orgID := r.URL.Query().Get("org")
    limit := parseInt(r.URL.Query().Get("limit"), 20)
    offset := parseInt(r.URL.Query().Get("offset"), 0)

    results := []SearchResult{}

    if strings.Contains(types, "devices") {
        rows, _ := h.pg.Query(r.Context(), `
            SELECT device_key, device_name, device_type, 'device' as entity_type
            FROM devices
            WHERE search_vector @@ plainto_tsquery('english', $1)
               OR device_key ILIKE '%' || $1 || '%'
            ORDER BY ts_rank(search_vector, plainto_tsquery('english', $1)) DESC
            LIMIT $2 OFFSET $3`, q, limit, offset)
        // scan rows into results
    }

    if strings.Contains(types, "users") {
        rows, _ := h.pg.Query(r.Context(), `
            SELECT id::text, email, display_name, 'user' as entity_type
            FROM users
            WHERE search_vector @@ plainto_tsquery('english', $1)
               OR email ILIKE '%' || $1 || '%'
            ORDER BY ts_rank(search_vector, plainto_tsquery('english', $1)) DESC
            LIMIT $2 OFFSET $3`, q, limit, offset)
        // scan rows into results
    }

    if strings.Contains(types, "orgs") {
        // similar query on organizations
    }

    if strings.Contains(types, "audit") && orgID != "" {
        rows, _ := h.pg.Query(r.Context(), `
            SELECT id::text, action, resource_type, resource_id, 'audit' as entity_type
            FROM audit_log
            WHERE org_id = $1
              AND search_vector @@ plainto_tsquery('english', $2)
            ORDER BY created_at DESC
            LIMIT $3 OFFSET $4`, orgID, q, limit, offset)
        // scan rows into results
    }

    writeJSON(w, 200, map[string]any{
        "results": results,
        "total":   len(results),
        "query":   q,
    })
}
```

Search uses PostgreSQL full-text search (tsvector) with GIN indexes. No external search service needed for v1. The generated columns update automatically on row changes. Queries use `plainto_tsquery` for simple input and `ts_rank` for relevance sorting. ILIKE fallback catches partial matches the FTS parser might miss.

## SMTP Configuration

```env
# .env — Email configuration
SMTP_HOST=smtp.sendgrid.net
SMTP_PORT=587
SMTP_USER=apikey
SMTP_PASS=SG.xxxxxxxxxxxx
SMTP_FROM=noreply@iotplatform.com
SMTP_FROM_NAME=IoT Platform
```

```go
// internal/config.go
type SMTPConfig struct {
    Host     string `env:"SMTP_HOST" required:"true"`
    Port     int    `env:"SMTP_PORT" default:"587"`
    User     string `env:"SMTP_USER" required:"true"`
    Pass     string `env:"SMTP_PASS" required:"true"`
    From     string `env:"SMTP_FROM" required:"true"`
    FromName string `env:"SMTP_FROM_NAME" default:"IoT Platform"`
}
```

## Health Check

```go
// internal/handlers.go — Health endpoint

func (h *Handlers) Health(w http.ResponseWriter, r *http.Request) {
    services := map[string]any{}

    // PostgreSQL
    pgStart := time.Now()
    if err := h.pg.Ping(r.Context()); err != nil {
        services["postgres"] = map[string]any{"status": "down", "error": err.Error()}
    } else {
        services["postgres"] = map[string]any{"status": "ok", "latency_ms": time.Since(pgStart).Milliseconds()}
    }

    // ClickHouse
    chStart := time.Now()
    if err := h.ch.Ping(r.Context()); err != nil {
        services["clickhouse"] = map[string]any{"status": "down", "error": err.Error()}
    } else {
        services["clickhouse"] = map[string]any{"status": "ok", "latency_ms": time.Since(chStart).Milliseconds()}
    }

    // Mosquitto
    if h.mqtt.IsConnected() {
        services["mosquitto"] = map[string]any{"status": "ok", "connected": true}
    } else {
        services["mosquitto"] = map[string]any{"status": "degraded", "connected": false}
    }

    // MinIO
    minioStart := time.Now()
    if _, err := h.minio.ListBuckets(r.Context()); err != nil {
        services["minio"] = map[string]any{"status": "down", "error": err.Error()}
    } else {
        services["minio"] = map[string]any{"status": "ok", "latency_ms": time.Since(minioStart).Milliseconds()}
    }

    overall := "ok"
    for _, s := range services {
        if s.(map[string]any)["status"] == "down" {
            overall = "degraded"
            break
        }
    }

    statusCode := 200
    if overall == "degraded" {
        statusCode = 503
    }

    writeJSON(w, statusCode, map[string]any{
        "status":         overall,
        "uptime_seconds": int(time.Since(startTime).Seconds()),
        "services":       services,
    })
}
```

## Data Retention (Per-Plan)

ClickHouse TTL is global (90 days hard cap). Per-plan retention (7d free, 90d pro, 365d business) is enforced by a cleanup job that deletes rows for devices whose owner's plan retention has expired.

```go
// internal/store.go — Retention cleanup (runs hourly via cron goroutine)

func (bw *BatchWriter) RetentionCleanup(ctx context.Context) error {
    // For each device, find the owner's plan retention_days.
    // Delete ClickHouse rows older than retention_days for that device.
    //
    // Done in one query per retention tier to avoid N device lookups:
    //   1. Find all device_ids whose owner is on a plan with retention_days < 90
    //   2. Group by retention_days
    //   3. For each group: ALTER TABLE ... DELETE WHERE device_id IN (...) AND ts < now() - retention

    rows, err := bw.pgPool.Query(ctx, `
        SELECT d.device_key, lp.retention_days
        FROM devices d
        JOIN user_licenses ul ON ul.user_id = d.owner_id
        JOIN license_plans lp ON lp.id = ul.plan_id
        WHERE lp.retention_days < 90
          AND d.is_active = true`)
    if err != nil {
        return fmt.Errorf("query device retentions: %w", err)
    }
    defer rows.Close()

    // Group device_keys by retention_days to batch the deletes
    byRetention := map[int][]string{}
    for rows.Next() {
        var key string
        var days int
        rows.Scan(&key, &days)
        byRetention[days] = append(byRetention[days], key)
    }

    for days, keys := range byRetention {
        cutoff := time.Now().AddDate(0, 0, -days)
        // ClickHouse ALTER TABLE ... DELETE is async; use lightweight delete
        bw.chPool.Exec(ctx, `
            DELETE FROM device_telemetry
            WHERE device_id IN (?)
              AND ts < ?`, keys, cutoff)
    }
    return nil
}

// Scheduled: runs at :17 every hour (off-peak, avoids :00 cluster)
// The 90-day ClickHouse TTL remains as a hard backstop regardless of plan.
```

## Data Export (GDPR)

Users can request a full export of their data. The job runs async (telemetry can be large) and emails a download link when ready.

```sql
CREATE TABLE export_jobs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending, running, ready, failed
    format          TEXT NOT NULL DEFAULT 'csv',       -- 'csv' or 'json'
    file_path       TEXT,                              -- MinIO path when ready
    row_count       INT,
    error           TEXT,
    requested_at    TIMESTAMPTZ DEFAULT now(),
    completed_at    TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ                         -- download link expires
);
```

```go
// internal/handlers.go — Export flow

// POST /api/export/request → kicks off async job, returns job_id
func (h *Handlers) RequestExport(w http.ResponseWriter, r *http.Request) {
    user := r.Context().Value("user").(*User)
    jobID := uuid.New()
    h.pg.Exec(r.Context(),
        `INSERT INTO export_jobs (id, user_id, status, expires_at)
         VALUES ($1, $2, 'pending', now() + interval '7 days')`,
        jobID, user.ID)
    go h.runExport(context.Background(), jobID, user)  // async
    writeJSON(w, 202, map[string]string{"job_id": jobID.String()})
}

// runExport queries all user data (profile, devices, config, telemetry sample)
// writes a zip to MinIO, marks job ready, emails download link.
func (h *Handlers) runExport(ctx context.Context, jobID uuid.UUID, user *User) {
    h.pg.Exec(ctx, `UPDATE export_jobs SET status = 'running' WHERE id = $1`, jobID)

    // 1. Query PostgreSQL: user, devices, device_config, commands, alerts
    // 2. Query ClickHouse: telemetry (capped at N rows or date range)
    // 3. Write CSV/JSON files into a zip
    // 4. Upload zip to MinIO at exports/{jobID}.zip
    // 5. Update job: status='ready', file_path, row_count, completed_at
    // 6. Email user: "Your data export is ready" with download link
}
```

The download endpoint (`GET /api/export/download/{job_id}`) validates the requester owns the job, checks `expires_at`, and streams the MinIO object with a `Content-Disposition: attachment` header.

## Billing / Invoicing (Manual)

Until Stripe integration is built, billing is managed manually through the admin dashboard.

### Flow

```
1. Admin creates invoice → POST /api/billing/invoices
   { org_id, plan_id, audience, period_start, period_end, amount_cents, description }

2. System generates invoice_number (INV-2026-XXXX)
   Status: pending

3. Customer pays via bank transfer (offline)

4. Admin marks paid → POST /api/billing/invoices/{id}/mark-paid
   ├─ Sets status = paid, paid_at = now()
   ├─ Updates user_licenses or org_licenses to the new plan
   ├─ Logs to license_change_log
   └─ Sends confirmation email to customer

5. If payment fails / customer cancels:
   ├─ Admin cancels → POST /api/billing/invoices/{id}/cancel
   └─ Status = cancelled, license unchanged
```

### License Auto-Update on Payment

```go
// internal/handlers.go — mark invoice as paid
func (h *Handlers) MarkInvoicePaid(w http.ResponseWriter, r *http.Request) {
    invoiceID := chi.URLParam(r, "id")

    tx, _ := h.pg.Begin(r.Context())
    defer tx.Rollback(r.Context())

    // 1. Get invoice
    var inv Invoice
    tx.QueryRow(r.Context(),
        `SELECT id, org_id, user_id, plan_id, audience, status FROM invoices WHERE id = $1`,
        invoiceID).Scan(&inv.ID, &inv.OrgID, &inv.UserID, &inv.PlanID, &inv.Audience, &inv.Status)

    if inv.Status != "pending" {
        http.Error(w, "invoice already processed", 400)
        return
    }

    // 2. Mark invoice paid
    tx.Exec(r.Context(),
        `UPDATE invoices SET status = 'paid', paid_at = now(), paid_via = 'manual' WHERE id = $1`,
        invoiceID)

    // 3. Update license
    if inv.Audience == "org" {
        tx.Exec(r.Context(),
            `INSERT INTO org_licenses (org_id, plan_id, device_count, starts_at)
             VALUES ($1, $2, 0, now())
             ON CONFLICT (org_id) DO UPDATE SET plan_id = $2, updated_at = now()`,
            inv.OrgID, inv.PlanID)
    } else {
        tx.Exec(r.Context(),
            `INSERT INTO user_licenses (user_id, plan_id, device_count, starts_at)
             VALUES ($1, $2, 0, now())
             ON CONFLICT (user_id) DO UPDATE SET plan_id = $2, updated_at = now()`,
            inv.UserID, inv.PlanID)
    }

    // 4. Log change
    tx.Exec(r.Context(),
        `INSERT INTO license_change_log (org_id, user_id, audience, to_plan_id, reason, invoice_id, changed_by)
         VALUES ($1, $2, $3, $4, 'payment_received', $5, $6)`,
        inv.OrgID, inv.UserID, inv.Audience, inv.PlanID, invoiceID, userID(r))

    tx.Commit(r.Context())

    // 5. Send confirmation email
    h.email.SendPaymentConfirmed(r.Context(), &inv)

    writeJSON(w, 200, map[string]string{"status": "paid"})
}
```

### Future Stripe Integration

When Stripe is added:
- `stripe_customer_id` on `organizations` links to Stripe customer
- `stripe_invoice_id` and `stripe_payment_intent_id` on `invoices` link to Stripe objects
- Stripe webhook handler updates invoice status and license automatically
- `payment_methods.stripe_id` stores Stripe payment method IDs
- Manual flow remains available for bank transfer / custom arrangements

## Deployment

### Docker Compose

```yaml
services:
  caddy:
    image: caddy:2-alpine
    ports: ["80:80", "443:443"]
    volumes:
      - caddy_data:/data
      - caddy_config:/config
      - ./Caddyfile:/etc/caddy/Caddyfile
    depends_on: [api, web]
    restart: unless-stopped

  api:
    build:
      context: ./backend
      dockerfile: Dockerfile.api
    depends_on:
      postgres: { condition: service_healthy }
      clickhouse: { condition: service_started }
      mosquitto: { condition: service_started }
      minio: { condition: service_started }
    env_file: .env
    environment:
      SERVICE_ROLE: api
    volumes:
      - ./backend/migrations:/migrations:ro
    restart: unless-stopped

  ingest:
    build:
      context: ./backend
      dockerfile: Dockerfile.ingest
    depends_on:
      postgres: { condition: service_healthy }
      clickhouse: { condition: service_started }
      mosquitto: { condition: service_started }
    env_file: .env
    environment:
      SERVICE_ROLE: ingest
    restart: unless-stopped

  mosquitto:
    image: eclipse-mosquitto:2
    volumes:
      - mqtt_data:/mosquitto/data
      - mqtt_log:/mosquitto/log
      - ./mosquitto/config:/mosquitto/config:ro
    restart: unless-stopped

  postgres:
    image: postgres:16-alpine
    environment:
      POSTGRES_DB: ${POSTGRES_DB}
      POSTGRES_USER: ${POSTGRES_USER}
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
    volumes:
      - pgdata:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U $$POSTGRES_USER"]
      interval: 10s
      timeout: 5s
      retries: 5
    restart: unless-stopped

  clickhouse:
    image: clickhouse/clickhouse-server:24.3-alpine
    ulimits:
      nofile: 262144
    volumes:
      - chdata:/var/lib/clickhouse
      - ./clickhouse/init:/docker-entrypoint-initdb.d:ro
    restart: unless-stopped

  minio:
    image: minio/minio
    environment:
      MINIO_ROOT_USER: ${MINIO_ROOT_USER}
      MINIO_ROOT_PASSWORD: ${MINIO_ROOT_PASSWORD}
    volumes:
      - minio_data:/data
    command: server /data --console-address ":9001"
    restart: unless-stopped

  web:
    build: ./web
    depends_on: [api]
    restart: unless-stopped

volumes:
  caddy_data:
  caddy_config:
  pgdata:
  chdata:
  mqtt_data:
  mqtt_log:
  minio_data:

networks:
  default:
    driver: bridge
```

### Database Migrations

Uses [golang-migrate](https://github.com/golang-migrate/migrate) — CLI runs migrations, Go embeds them for in-app runs.

```bash
# Install CLI
go install -tags 'postgres' github.com/golang-migrate/migrate/v4/cmd/migrate@latest

# Create a migration
migrate create -ext sql -dir migrations -seq add_device_groups

# Apply (dev)
make migrate-up    # migrate -path migrations -database $DATABASE_URL up

# Roll back
make migrate-down  # ... down
```

Migration files live in `api/migrations/` as `NNNNNN_name.up.sql` / `NNNNNN_name.down.sql`. The API binary embeds them (`go:embed migrations/*.sql`) and runs pending migrations on startup if `AUTO_MIGRATE=true`.

### CI/CD (GitHub Actions)

```yaml
# .github/workflows/deploy.yml
on:
  push:
    branches: [main]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
      - run: make test
      - run: make lint

  deploy:
    needs: test
    runs-on: ubuntu-latest
    steps:
      - run: docker compose build
      - run: docker compose push
      - run: ssh deploy@host "docker compose pull && docker compose up -d"
```

## Backups

Three data stores need backup: PostgreSQL (relational), ClickHouse (telemetry), MinIO (firmware binaries + exports). Backups run via cron containers in Docker Compose.

### PostgreSQL (RPO: 1 hour)

```yaml
# docker-compose.yml — backup service
  pgbackup:
    image: prodrigestivill/postgres-backup-local:16
    depends_on: [postgres]
    environment:
      POSTGRES_HOST: postgres
      POSTGRES_DB: ${POSTGRES_DB}
      POSTGRES_USER: ${POSTGRES_USER}
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
      SCHEDULE: "@hourly"
      BACKUP_KEEP_DAYS: 7
      BACKUP_KEEP_WEEKS: 4
      BACKUP_KEEP_MONTHS: 6
    volumes:
      - ./backups/postgres:/backups
```

Dump format: gzipped custom (`pg_dump -Fc`). Retention: 7 daily, 4 weekly, 6 monthly. Restore: `pg_restore -Fc -d powermon < dump.gz`.

### ClickHouse (RPO: 6 hours)

ClickHouse backups use `clickhouse-backup` (native tool). Telemetry is less critical than relational data (regenerable from devices if lost, and devices re-publish), so 6h RPO is acceptable.

```yaml
  chbackup:
    image: altinity/clickhouse-backup:latest
    depends_on: [clickhouse]
    volumes:
      - chdata:/var/lib/clickhouse
      - ./backups/clickhouse:/backups
    environment:
      BACKUP_SCHEDULE: "0 */6 * * *"
      KEEP_LAST_BACKUPS: 4
```

### MinIO (RPO: daily)

Firmware binaries are versioned and rarely change. Daily mirror to a second MinIO bucket or S3-compatible remote (Wasabi/Backblaze).

```bash
# Cron job on host: mirror MinIO to remote
mc alias set local http://minio:9000 $MINIO_ROOT_USER $MINIO_ROOT_PASSWORD
mc alias set remote https://s3.wasabisys.com $REMOTE_KEY $REMOTE_SECRET
mc mirror --overwrite --watch local/firmware remote/firmware-backup
```

### Restore procedure

| Store | Command | RTO |
|---|---|---|
| PostgreSQL | `pg_restore -Fc -d powermon < dump.gz` | < 30 min |
| ClickHouse | `clickhouse-backup restore <name>` | < 1 h |
| MinIO | `mc mirror remote/firmware-backup local/firmware` | < 15 min |

### Offsite

All three backup directories (`./backups/`) should sync to offsite storage (rsync to a VPS, or rclone to S3) daily. The backup containers write locally; a host cron ships them offsite.

## Security

- Passwords: bcrypt cost 12
- JWT: access token 15min, refresh token 7d, rotation on use
- Device auth: API key as bearer token, separate from user JWT
- MQTT auth: Mosquitto HTTP plugin → Go API validates device credentials
- SQL: parameterized queries everywhere
- Input: validated on every endpoint
- CORS: strict origin whitelist
- Headers: Content-Security-Policy, X-Content-Type-Options, etc.
- Rate limiting: per-IP on auth, per-token on API
- Secrets: environment variables only, never in code or logs
- Log sanitization: passwords, tokens, API keys redacted before writing

## Dashboard-Configurable Settings

The following are managed through the API/dashboard, not code changes:

| Setting | Table | Change Method |
|---|---|---|
| License plans | `license_plans` | API → PG |
| Device type schemas | `device_types.schema_def` | API → PG |
| Alert rules | `alert_rules` | API → PG |
| OTA channels | `ota_releases.channel` | API → PG |
| Feature flags | `license_plans.features` | API → PG |
| Email templates | `email_templates` | API → PG |
| Org SSO providers | `org_oauth_providers` | API → PG |
| Maintenance mode | `maintenance_mode` | API → PG |
| Invoices | `invoices` | API → PG (admin) |
| Payment methods | `payment_methods` | API → PG (admin) |
| Device groups | `device_groups` | API → PG |
| Device tags | `device_tags` | API → PG |
| Notification prefs | `notification_preferences` | API → PG |
| Rate limits | `license_plans` (per-plan) | API → PG |

## Go Caveats Addressed

| Pitfall | Mitigation |
|---|---|
| Loop variable capture | Go 1.22+ fix + linter |
| Unbounded goroutines | Fixed-size worker pool |
| Map concurrent access | sync.RWMutex on all shared maps |
| JSON numbers → float64 | json.Decoder + UseNumber() |
| Default HTTP client | Custom client with 10s timeout |
| SQL injection | Parameterized queries, banned fmt.Sprintf in SQL |
| Secrets in logs | Sanitize() function on all log calls |
| Context not checked | ctx.Done() in all long-running loops |
| Defer errors | Named returns + explicit checks |
| Goroutine leaks | errgroup with context cancellation |
| Docker CPU limits | uber-go/automaxprocs |
| Time zone confusion | All times UTC, API accepts ISO 8601 |
