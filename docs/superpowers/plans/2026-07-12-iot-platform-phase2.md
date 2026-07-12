# IoT Platform — Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add OTA firmware updates, alert engine, email service, device groups/tags, search, rate limiting, and notification preferences.

**Architecture:** All new code lives in the API server (`cmd/api`). The ingest worker is unchanged (pure plumbing). OTA check endpoint is polled by ESP32s. Alert engine evaluates rules on the `live/#` stream. Email is async via `email_queue` table. Search uses PostgreSQL FTS.

**Spec:** `docs/superpowers/specs/2026-07-12-iot-platform-backend-design.md`

---

## File Structure (additions)

```
backend/
├── internal/
│   ├── ota.go                  # OTA release management + check endpoint
│   ├── ota_test.go
│   ├── alerts.go               # Alert rule evaluation on live/# stream
│   ├── alerts_test.go
│   ├── email.go                # Email queue + template rendering + SMTP
│   ├── email_test.go
│   ├── search.go               # Full-text search handlers
│   ├── search_test.go
│   ├── groups.go               # Device groups/tags CRUD
│   ├── groups_test.go
│   ├── ratelimit.go            # Token bucket rate limiter
│   ├── ratelimit_test.go
│   └── fakes/
│       ├── email.go            # FakeSender — captures sent emails
│       └── builders.go         # Add anAlertRule(), aGroup() builders
├── migrations/
│   ├── 002_ota_alerts.up.sql
│   └── 002_ota_alerts.down.sql
├── clickhouse/
│   └── init/
│       └── 002_mv_hourly.sql   # Hourly aggregate materialized view
```

---

### Task 1: Migration 002 — OTA, alerts, groups, search, prefs

**Files:**
- Create: `backend/migrations/002_ota_alerts.up.sql`
- Create: `backend/migrations/002_ota_alerts.down.sql`

- [ ] **Step 1: Write `migrations/002_ota_alerts.up.sql`**

```sql
-- Phase 2: OTA releases, alert rules, device groups, search, notification prefs

-- ============================================================
-- OTA Releases
-- ============================================================
CREATE TABLE ota_releases (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
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
    UNIQUE (device_type, version)
);

-- ============================================================
-- Alert Rules
-- ============================================================
CREATE TABLE alert_rules (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            TEXT NOT NULL,
    description     TEXT,
    device_type     TEXT,
    device_key      TEXT,
    enabled         BOOLEAN DEFAULT true,
    field           TEXT NOT NULL,
    operator        TEXT NOT NULL,  -- 'gt', 'lt', 'gte', 'lte', 'eq', 'neq'
    value           FLOAT NOT NULL,
    duration_sec    INT NOT NULL DEFAULT 0,
    notify_email    BOOLEAN DEFAULT true,
    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- Alert Events
-- ============================================================
CREATE TABLE alert_events (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    rule_id         UUID NOT NULL REFERENCES alert_rules(id) ON DELETE CASCADE,
    device_key      TEXT NOT NULL,
    status          TEXT NOT NULL DEFAULT 'firing',  -- firing, resolved, acknowledged
    fired_at        TIMESTAMPTZ DEFAULT now(),
    resolved_at     TIMESTAMPTZ,
    fired_value     FLOAT,
    resolved_value  FLOAT,
    notified_at     TIMESTAMPTZ
);

CREATE INDEX idx_alert_events_rule_status ON alert_events (rule_id, status);
CREATE INDEX idx_alert_events_device ON alert_events (device_key, fired_at DESC);

-- ============================================================
-- Email Templates
-- ============================================================
CREATE TABLE email_templates (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    template_key    TEXT UNIQUE NOT NULL,
    subject         TEXT NOT NULL,
    body_text       TEXT NOT NULL,
    body_html       TEXT NOT NULL,
    variables       TEXT[] NOT NULL DEFAULT '{}',
    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

INSERT INTO email_templates (template_key, subject, body_text, body_html, variables) VALUES
('welcome',
 'Welcome to IoT Platform',
 'Hi {{.DisplayName}},\n\nWelcome to IoT Platform!',
 '<h1>Welcome</h1><p>Hi {{.DisplayName}},</p>',
 ARRAY['DisplayName', 'PlatformName']),
('alert_fired',
 'ALERT: {{.RuleName}} — {{.DeviceName}}',
 'Alert "{{.RuleName}}" fired for {{.DeviceName}} ({{.DeviceKey}}).\nValue: {{.Value}}\nThreshold: {{.Threshold}}',
 '<h2>Alert: {{.RuleName}}</h2><p>Device: <strong>{{.DeviceName}}</strong></p><p>Value: {{.Value}}</p>',
 ARRAY['RuleName', 'DeviceName', 'DeviceKey', 'Value', 'Threshold']),
('alert_resolved',
 'RESOLVED: {{.RuleName}} — {{.DeviceName}}',
 'Alert "{{.RuleName}}" for {{.DeviceName}} has resolved.\nValue: {{.Value}}',
 '<h2>Resolved: {{.RuleName}}</h2><p>Device: <strong>{{.DeviceName}}</strong></p>',
 ARRAY['RuleName', 'DeviceName', 'DeviceKey', 'Value']);

-- ============================================================
-- Email Queue
-- ============================================================
CREATE TABLE email_queue (
    id              BIGSERIAL PRIMARY KEY,
    template_key    TEXT NOT NULL,
    recipient       TEXT NOT NULL,
    user_id         UUID REFERENCES users(id) ON DELETE SET NULL,
    data            JSONB NOT NULL DEFAULT '{}',
    status          TEXT NOT NULL DEFAULT 'queued',  -- queued, sending, sent, failed
    attempts        INT NOT NULL DEFAULT 0,
    last_error      TEXT,
    next_attempt_at TIMESTAMPTZ DEFAULT now(),
    queued_at       TIMESTAMPTZ DEFAULT now(),
    sent_at         TIMESTAMPTZ
);

CREATE INDEX idx_email_queue_due ON email_queue (status, next_attempt_at)
    WHERE status IN ('queued', 'sending');

-- ============================================================
-- Device Groups
-- ============================================================
CREATE TABLE device_groups (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            TEXT NOT NULL,
    description     TEXT,
    color           TEXT,
    created_at      TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE device_group_members (
    group_id        UUID REFERENCES device_groups(id) ON DELETE CASCADE,
    device_key      TEXT REFERENCES devices(device_key) ON DELETE CASCADE,
    added_at        TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (group_id, device_key)
);

-- ============================================================
-- Device Tags
-- ============================================================
CREATE TABLE device_tags (
    device_key      TEXT REFERENCES devices(device_key) ON DELETE CASCADE,
    key             TEXT NOT NULL,
    value           TEXT NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ DEFAULT now(),
    PRIMARY KEY (device_key, key)
);

-- ============================================================
-- Notification Preferences
-- ============================================================
CREATE TABLE notification_preferences (
    user_id             UUID PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    alert_fired_email   BOOLEAN DEFAULT true,
    alert_resolved_email BOOLEAN DEFAULT true,
    quiet_hours_start   INT,
    quiet_hours_end     INT,
    created_at          TIMESTAMPTZ DEFAULT now(),
    updated_at          TIMESTAMPTZ DEFAULT now()
);

-- Seed defaults for all existing users
INSERT INTO notification_preferences (user_id)
SELECT id FROM users
ON CONFLICT (user_id) DO NOTHING;

-- ============================================================
-- Full-Text Search
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

- [ ] **Step 2: Write `migrations/002_ota_alerts.down.sql`**

```sql
DROP INDEX IF EXISTS idx_audit_search;
DROP INDEX IF EXISTS idx_devices_search;
ALTER TABLE devices DROP COLUMN IF EXISTS search_vector;
ALTER TABLE audit_log DROP COLUMN IF EXISTS search_vector;
DROP TABLE IF EXISTS notification_preferences;
DROP TABLE IF EXISTS device_tags;
DROP TABLE IF EXISTS device_group_members;
DROP TABLE IF EXISTS device_groups;
DROP TABLE IF EXISTS email_queue;
DROP TABLE IF EXISTS email_templates;
DROP TABLE IF EXISTS alert_events;
DROP TABLE IF EXISTS alert_rules;
DROP TABLE IF EXISTS ota_releases;
```

- [ ] **Step 3: Commit**

```bash
git add backend/migrations/002_ota_alerts.up.sql backend/migrations/002_ota_alerts.down.sql
git commit -m "feat: migration 002 — OTA, alerts, groups, search, prefs"
```

---

### Task 2: OTA release management + check endpoint

**Files:**
- Create: `backend/internal/ota.go`
- Create: `backend/internal/ota_test.go`

- [ ] **Step 1: Write `internal/ota.go`**

```go
// internal/ota.go — OTA firmware release management.
// Orgs create releases; devices poll the check endpoint to find updates.

package internal

import (
    "encoding/json"
    "net/http"
    "time"

    "github.com/go-chi/chi/v5"
    "github.com/jackc/pgx/v5/pgxpool"
)

type OTAHandler struct {
    pg *pgxpool.Pool
}

type OTACheckResponse struct {
    UpdateAvailable bool   `json:"update_available"`
    Version         string `json:"version,omitempty"`
    BinaryURL       string `json:"binary_url,omitempty"`
    SHA256          string `json:"sha256,omitempty"`
    BinarySize      int    `json:"binary_size,omitempty"`
}

func NewOTAHandler(pg *pgxpool.Pool) *OTAHandler {
    return &OTAHandler{pg: pg}
}

// CheckOTA is polled by ESP32: GET /api/v1/ota/check/{key}?current_ver=2.0.0
func (h *OTAHandler) CheckOTA(w http.ResponseWriter, r *http.Request) {
    deviceKey := chi.URLParam(r, "key")
    currentVer := r.URL.Query().Get("current_ver")

    var deviceType string
    err := h.pg.QueryRow(r.Context(),
        `SELECT device_type FROM devices WHERE device_key = $1 AND is_active = true`,
        deviceKey).Scan(&deviceType)
    if err != nil {
        writeError(w, "not_found", "device not found", http.StatusNotFound)
        return
    }

    var release struct {
        Version    string
        BinaryPath string
        SHA256     string
        BinarySize int
    }
    err = h.pg.QueryRow(r.Context(), `
        SELECT version, binary_path, sha256, binary_size
        FROM ota_releases
        WHERE device_type = $1 AND channel = 'stable'
          AND version > $2
        ORDER BY created_at DESC LIMIT 1`,
        deviceType, currentVer).Scan(&release.Version, &release.BinaryPath, &release.SHA256, &release.BinarySize)
    if err != nil {
        writeJSON(w, http.StatusOK, OTACheckResponse{UpdateAvailable: false})
        return
    }

    writeJSON(w, http.StatusOK, OTACheckResponse{
        UpdateAvailable: true,
        Version:         release.Version,
        BinaryURL:       "http://minio:9000/firmware/" + release.BinaryPath,
        SHA256:          release.SHA256,
        BinarySize:      release.BinarySize,
    })
}

// CreateRelease is an admin endpoint: POST /api/v1/ota/releases
func (h *OTAHandler) CreateRelease(w http.ResponseWriter, r *http.Request) {
    var req struct {
        DeviceType string `json:"device_type"`
        Version    string `json:"version"`
        Channel    string `json:"channel"`
        BinaryPath string `json:"binary_path"`
        BinarySize int    `json:"binary_size"`
        SHA256     string `json:"sha256"`
        Changelog  string `json:"changelog"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
        writeError(w, "bad_request", "invalid request body", http.StatusBadRequest)
        return
    }

    _, err := h.pg.Exec(r.Context(), `
        INSERT INTO ota_releases (device_type, version, channel, binary_path, binary_size, sha256, changelog)
        VALUES ($1, $2, $3, $4, $5, $6, $7)`,
        req.DeviceType, req.Version, req.Channel, req.BinaryPath, req.BinarySize, req.SHA256, req.Changelog)
    if err != nil {
        writeError(w, "conflict", "release version already exists", http.StatusConflict)
        return
    }
    writeJSON(w, http.StatusCreated, map[string]string{"status": "created"})
}
```

- [ ] **Step 2: Write `internal/ota_test.go`**

```go
// backend/internal/ota_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "testing"

    "github.com/go-chi/chi/v5"
)

func TestOTACheck_NoUpdate(t *testing.T) {
    h := &OTAHandler{}
    r := chi.NewRouter()
    r.Get("/api/v1/ota/check/{key}", h.CheckOTA)

    req := httptest.NewRequest("GET", "/api/v1/ota/check/AABBCCDDEEFF?current_ver=2.0.0", nil)
    rec := httptest.NewRecorder()
    r.ServeHTTP(rec, req)

    if rec.Code != http.StatusNotFound && rec.Code != http.StatusOK {
        t.Errorf("status = %d, want 200 or 404", rec.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestOTA ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire OTA routes in cmd/api/main.go**

```go
// Add to the router in cmd/api/main.go, inside the protected group:
r.Get("/ota/check/{key}", ota.CheckOTA)
r.Post("/ota/releases", ota.CreateRelease)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/ota.go backend/internal/ota_test.go
git commit -m "feat: OTA release management and device check endpoint"
```

---

### Task 3: Alert engine (evaluates rules on live/# stream)

**Files:**
- Create: `backend/internal/alerts.go`
- Create: `backend/internal/alerts_test.go`

- [ ] **Step 1: Write `internal/alerts.go`**

```go
// internal/alerts.go — Alert rule evaluation. Runs in the API server,
// triggered by each message on the live/# MQTT stream. Evaluates rules
// against the enriched payload and fires/resolves events.

package internal

import (
    "context"
    "encoding/json"
    "fmt"
    "log/slog"
    "sync"
    "time"

    "github.com/jackc/pgx/v5/pgxpool"
)

type AlertEngine struct {
    pg       *pgxpool.Pool
    email    *EmailService
    mu       sync.RWMutex
    counters map[string]int // "rule_id:device_key" → consecutive match count
}

func NewAlertEngine(pg *pgxpool.Pool, email *EmailService) *AlertEngine {
    return &AlertEngine{
        pg:       pg,
        email:    email,
        counters: make(map[string]int),
    }
}

// Evaluate checks all active rules against the enriched telemetry.
// Called by the live/# handler for each incoming message.
func (e *AlertEngine) Evaluate(ctx context.Context, deviceKey string, enriched *EnrichedTelemetry) {
    rules, err := e.loadRules(ctx)
    if err != nil {
        slog.Error("load rules", "error", err)
        return
    }

    for _, rule := range rules {
        if !e.matchesDevice(rule, deviceKey) {
            continue
        }
        rawValue := e.getFieldValue(enriched, rule.Field)
        matched := e.evaluateCondition(rawValue, rule.Operator, rule.Value)

        key := rule.ID + ":" + deviceKey
        e.mu.Lock()
        if matched {
            e.counters[key]++
        } else {
            delete(e.counters, key)
        }
        count := e.counters[key]
        e.mu.Unlock()

        if matched && count >= requiredSamples(rule.DurationSec) {
            e.fire(ctx, rule, deviceKey, rawValue)
        } else if !matched {
            e.resolve(ctx, rule, deviceKey, rawValue)
        }
    }
}

type alertRule struct {
    ID          string
    Name        string
    DeviceType  string
    DeviceKey   string
    Enabled     bool
    Field       string
    Operator    string
    Value       float64
    DurationSec int
    NotifyEmail bool
}

func (e *AlertEngine) loadRules(ctx context.Context) ([]alertRule, error) {
    rows, err := e.pg.Query(ctx, `
        SELECT id, name, coalesce(device_type,''), coalesce(device_key,''),
               enabled, field, operator, value, duration_sec, notify_email
        FROM alert_rules WHERE enabled = true`)
    if err != nil {
        return nil, fmt.Errorf("query rules: %w", err)
    }
    defer rows.Close()

    var rules []alertRule
    for rows.Next() {
        var r alertRule
        rows.Scan(&r.ID, &r.Name, &r.DeviceType, &r.DeviceKey,
            &r.Enabled, &r.Field, &r.Operator, &r.Value, &r.DurationSec, &r.NotifyEmail)
        rules = append(rules, r)
    }
    return rules, nil
}

func (e *AlertEngine) matchesDevice(rule alertRule, deviceKey string) bool {
    if rule.DeviceKey != "" && rule.DeviceKey != deviceKey {
        return false
    }
    return true
}

func (e *AlertEngine) getFieldValue(enriched *EnrichedTelemetry, field string) float64 {
    switch field {
    case "pv_power":
        return float64(enriched.PVPower)
    case "battery_power":
        return float64(enriched.BatteryPower)
    case "inverter_power":
        return float64(enriched.InverterPower)
    case "dc_load_power":
        return float64(enriched.DCLoadPower)
    case "min_soc_pct":
        return float64(enriched.MinSOCPct)
    case "max_soc_pct":
        return float64(enriched.MaxSOCPct)
    default:
        if v, ok := enriched.Fields[field]; ok {
            return v
        }
        return 0
    }
}

func (e *AlertEngine) evaluateCondition(value float64, op string, threshold float64) bool {
    switch op {
    case "gt":
        return value > threshold
    case "lt":
        return value < threshold
    case "gte":
        return value >= threshold
    case "lte":
        return value <= threshold
    case "eq":
        return value == threshold
    case "neq":
        return value != threshold
    default:
        return false
    }
}

func requiredSamples(durationSec int) int {
    if durationSec <= 0 {
        return 1
    }
    return (durationSec + 4) / 5 // ceil(duration / 5s interval)
}

func (e *AlertEngine) fire(ctx context.Context, rule alertRule, deviceKey string, value float64) {
    // Check not already firing
    var existing string
    err := e.pg.QueryRow(ctx,
        `SELECT id FROM alert_events WHERE rule_id = $1 AND device_key = $2 AND status = 'firing' LIMIT 1`,
        rule.ID, deviceKey).Scan(&existing)
    if err == nil {
        return // already firing
    }

    e.pg.Exec(ctx,
        `INSERT INTO alert_events (rule_id, device_key, status, fired_value) VALUES ($1, $2, 'firing', $3)`,
        rule.ID, deviceKey, value)

    if rule.NotifyEmail {
        e.email.Enqueue(ctx, "alert_fired", deviceKey, "", map[string]any{
            "RuleName":   rule.Name,
            "DeviceKey":  deviceKey,
            "Value":      value,
            "Threshold":  rule.Value,
        })
    }
    slog.Warn("alert fired", "rule", rule.Name, "device", deviceKey, "value", value)
}

func (e *AlertEngine) resolve(ctx context.Context, rule alertRule, deviceKey string, value float64) {
    tag, err := e.pg.Exec(ctx,
        `UPDATE alert_events SET status = 'resolved', resolved_at = now(), resolved_value = $3
         WHERE rule_id = $1 AND device_key = $2 AND status = 'firing'`,
        rule.ID, deviceKey, value)
    if err != nil || tag.RowsAffected() == 0 {
        return
    }
    if rule.NotifyEmail {
        e.email.Enqueue(ctx, "alert_resolved", deviceKey, "", map[string]any{
            "RuleName":   rule.Name,
            "DeviceKey":  deviceKey,
            "Value":      value,
        })
    }
    slog.Info("alert resolved", "rule", rule.Name, "device", deviceKey)
}
```

- [ ] **Step 2: Write `internal/alerts_test.go`**

```go
// backend/internal/alerts_test.go
package internal

import (
    "testing"
)

func TestEvaluateCondition(t *testing.T) {
    e := &AlertEngine{}
    tests := []struct {
        value     float64
        op        string
        threshold float64
        want      bool
    }{
        {10, "gt", 5, true},
        {3, "gt", 5, false},
        {5, "gte", 5, true},
        {5, "lt", 10, true},
        {5, "eq", 5, true},
        {5, "neq", 10, true},
    }
    for _, tt := range tests {
        got := e.evaluateCondition(tt.value, tt.op, tt.threshold)
        if got != tt.want {
            t.Errorf("evaluateCondition(%v, %q, %v) = %v, want %v", tt.value, tt.op, tt.threshold, got, tt.want)
        }
    }
}

func TestRequiredSamples(t *testing.T) {
    if got := requiredSamples(0); got != 1 {
        t.Errorf("requiredSamples(0) = %d, want 1", got)
    }
    if got := requiredSamples(5); got != 1 {
        t.Errorf("requiredSamples(5) = %d, want 1", got)
    }
    if got := requiredSamples(6); got != 2 {
        t.Errorf("requiredSamples(6) = %d, want 2", got)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestEvaluate\|TestRequired ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire alert engine into WebSocket hub's OnLiveMessage**

```go
// In cmd/api/main.go, create the alert engine and pass it to the hub:
alertEngine := internal.NewAlertEngine(pg, emailSvc)
// Modify hub.OnLiveMessage to call alertEngine.Evaluate()
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/alerts.go backend/internal/alerts_test.go
git commit -m "feat: alert engine — rule evaluation on live/# stream"
```

---

### Task 4: Email service (async queue + SMTP)

**Files:**
- Create: `backend/internal/email.go`
- Create: `backend/internal/email_test.go`

- [ ] **Step 1: Write `internal/email.go`**

```go
// internal/email.go — Async email service. Triggering code enqueues
// a row into email_queue; a background goroutine drains it, renders
// the template, and sends via SMTP with retry.

package internal

import (
    "bytes"
    "context"
    "encoding/json"
    "html/template"
    "log/slog"
    "time"

    "github.com/jackc/pgx/v5/pgxpool"
    "gopkg.in/gomail.v2"
)

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

func NewEmailService(pg *pgxpool.Pool, fromAddr, smtpHost string, smtpPort int, smtpUser, smtpPass string) *EmailService {
    return &EmailService{
        pg:       pg,
        fromAddr: fromAddr,
        smtpHost: smtpHost,
        smtpPort: smtpPort,
        smtpUser: smtpUser,
        smtpPass: smtpPass,
        platform: "IoT Platform",
        baseURL:  "http://localhost:3000",
    }
}

// Enqueue inserts an email into the queue. Returns immediately.
func (e *EmailService) Enqueue(ctx context.Context, templateKey, recipient, userID string, data map[string]any) {
    payload, _ := json.Marshal(data)
    e.pg.Exec(ctx,
        `INSERT INTO email_queue (template_key, recipient, user_id, data) VALUES ($1, $2, $3, $4)`,
        templateKey, recipient, userID, payload)
}

// DrainLoop runs as a background goroutine, polling the queue every 5s.
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
    rows, err := e.pg.Query(ctx, `
        SELECT id, template_key, recipient, data FROM email_queue
        WHERE status = 'queued' AND next_attempt_at <= now()
        ORDER BY next_attempt_at LIMIT 10
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
    e.pg.Exec(ctx, `UPDATE email_queue SET status='sending', attempts=attempts+1 WHERE id=$1`, id)

    tmpl, err := e.loadTemplate(ctx, key)
    if err != nil {
        e.failOne(ctx, id, err)
        return
    }

    var vars map[string]any
    json.Unmarshal(data, &vars)
    vars["PlatformName"] = e.platform

    subject := renderText(tmpl.Subject, vars)
    bodyHTML := renderText(tmpl.BodyHTML, vars)
    bodyText := renderText(tmpl.BodyText, vars)

    msg := gomail.NewMessage()
    msg.SetHeader("From", e.fromAddr)
    msg.SetHeader("To", recipient)
    msg.SetHeader("Subject", subject)
    msg.SetBody("text/plain", bodyText)
    msg.AddAlternative("text/html", bodyHTML)

    dialer := gomail.NewDialer(e.smtpHost, e.smtpPort, e.smtpUser, e.smtpPass)
    if err := dialer.DialAndSend(msg); err != nil {
        e.failOne(ctx, id, err)
        return
    }
    e.pg.Exec(ctx, `UPDATE email_queue SET status='sent', sent_at=now() WHERE id=$1`, id)
}

type emailTemplate struct {
    Subject   string
    BodyText  string
    BodyHTML  string
}

func (e *EmailService) loadTemplate(ctx context.Context, key string) (*emailTemplate, error) {
    var t emailTemplate
    err := e.pg.QueryRow(ctx,
        `SELECT subject, body_text, body_html FROM email_templates WHERE template_key = $1`, key).
        Scan(&t.Subject, &t.BodyText, &t.BodyHTML)
    if err != nil {
        return nil, err
    }
    return &t, nil
}

func (e *EmailService) failOne(ctx context.Context, id int64, err error) {
    e.pg.Exec(ctx, `
        UPDATE email_queue
        SET status = CASE WHEN attempts >= 3 THEN 'failed' ELSE 'queued' END,
            last_error = $2,
            next_attempt_at = now() + (interval '1 minute' * (5 ^ attempts))
        WHERE id = $1`, id, err.Error())
    slog.Error("email send failed", "id", id, "error", err)
}

func renderText(tmpl string, vars map[string]any) string {
    t, err := template.New("").Parse(tmpl)
    if err != nil {
        return tmpl
    }
    var buf bytes.Buffer
    t.Execute(&buf, vars)
    return buf.String()
}
```

- [ ] **Step 2: Write `internal/email_test.go`**

```go
// backend/internal/email_test.go
package internal

import (
    "testing"
)

func TestRenderText(t *testing.T) {
    result := renderText("Hello {{.Name}}", map[string]any{"Name": "World"})
    if result != "Hello World" {
        t.Errorf("renderText = %q, want 'Hello World'", result)
    }
}

func TestRenderText_MissingVar(t *testing.T) {
    result := renderText("Hello {{.Name}}", nil)
    if result != "Hello <no value>" {
        t.Errorf("renderText = %q, want 'Hello <no value>'", result)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestRender ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire email service in cmd/api/main.go**

```go
// In cmd/api/main.go, create the email service and start the drain loop:
emailSvc := internal.NewEmailService(pg, cfg.SMTPFrom, cfg.SMTPHost, cfg.SMTPPort, cfg.SMTPUser, cfg.SMTPPass)
go emailSvc.DrainLoop(ctx)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/email.go backend/internal/email_test.go
git commit -m "feat: async email service with queue, templates, SMTP"
```

---

### Task 5: Device groups/tags CRUD

**Files:**
- Create: `backend/internal/groups.go`
- Create: `backend/internal/groups_test.go`

- [ ] **Step 1: Write `internal/groups.go`**

```go
// internal/groups.go — Device groups and tags CRUD.
// Groups organize devices; tags attach key-value metadata.

package internal

import (
    "encoding/json"
    "net/http"

    "github.com/go-chi/chi/v5"
    "github.com/jackc/pgx/v5/pgxpool"
)

type GroupHandler struct {
    pg *pgxpool.Pool
}

func NewGroupHandler(pg *pgxpool.Pool) *GroupHandler {
    return &GroupHandler{pg: pg}
}

func (h *GroupHandler) ListGroups(w http.ResponseWriter, r *http.Request) {
    rows, err := h.pg.Query(r.Context(),
        `SELECT id, name, coalesce(description,''), coalesce(color,'') FROM device_groups ORDER BY name`)
    if err != nil {
        writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
        return
    }
    defer rows.Close()

    type Group struct {
        ID          string `json:"id"`
        Name        string `json:"name"`
        Description string `json:"description,omitempty"`
        Color       string `json:"color,omitempty"`
    }
    groups := []Group{}
    for rows.Next() {
        var g Group
        rows.Scan(&g.ID, &g.Name, &g.Description, &g.Color)
        groups = append(groups, g)
    }
    writeJSON(w, http.StatusOK, groups)
}

func (h *GroupHandler) CreateGroup(w http.ResponseWriter, r *http.Request) {
    var req struct {
        Name        string `json:"name"`
        Description string `json:"description"`
        Color       string `json:"color"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Name == "" {
        writeError(w, "validation_error", "name is required", http.StatusBadRequest)
        return
    }
    var id string
    err := h.pg.QueryRow(r.Context(),
        `INSERT INTO device_groups (name, description, color) VALUES ($1, $2, $3) RETURNING id`,
        req.Name, req.Description, req.Color).Scan(&id)
    if err != nil {
        writeError(w, "conflict", "group name already exists", http.StatusConflict)
        return
    }
    writeJSON(w, http.StatusCreated, map[string]string{"id": id})
}

func (h *GroupHandler) AddDeviceToGroup(w http.ResponseWriter, r *http.Request) {
    groupID := chi.URLParam(r, "id")
    deviceKey := chi.URLParam(r, "key")
    _, err := h.pg.Exec(r.Context(),
        `INSERT INTO device_group_members (group_id, device_key) VALUES ($1, $2) ON CONFLICT DO NOTHING`,
        groupID, deviceKey)
    if err != nil {
        writeError(w, "internal_error", "failed to add device", http.StatusInternalServerError)
        return
    }
    writeJSON(w, http.StatusOK, map[string]string{"status": "added"})
}

func (h *GroupHandler) RemoveDeviceFromGroup(w http.ResponseWriter, r *http.Request) {
    groupID := chi.URLParam(r, "id")
    deviceKey := chi.URLParam(r, "key")
    h.pg.Exec(r.Context(),
        `DELETE FROM device_group_members WHERE group_id = $1 AND device_key = $2`,
        groupID, deviceKey)
    writeJSON(w, http.StatusOK, map[string]string{"status": "removed"})
}

// ── Tags ────────────────────────────────────────────────────────────

func (h *GroupHandler) ListTags(w http.ResponseWriter, r *http.Request) {
    deviceKey := chi.URLParam(r, "key")
    rows, err := h.pg.Query(r.Context(),
        `SELECT key, value FROM device_tags WHERE device_key = $1 ORDER BY key`, deviceKey)
    if err != nil {
        writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
        return
    }
    defer rows.Close()
    tags := map[string]string{}
    for rows.Next() {
        var k, v string
        rows.Scan(&k, &v)
        tags[k] = v
    }
    writeJSON(w, http.StatusOK, tags)
}

func (h *GroupHandler) SetTag(w http.ResponseWriter, r *http.Request) {
    deviceKey := chi.URLParam(r, "key")
    tagKey := chi.URLParam(r, "tag_key")
    var req struct {
        Value string `json:"value"`
    }
    json.NewDecoder(r.Body).Decode(&req)
    h.pg.Exec(r.Context(),
        `INSERT INTO device_tags (device_key, key, value) VALUES ($1, $2, $3)
         ON CONFLICT (device_key, key) DO UPDATE SET value = $3`,
        deviceKey, tagKey, req.Value)
    writeJSON(w, http.StatusOK, map[string]string{"status": "set"})
}

func (h *GroupHandler) DeleteTag(w http.ResponseWriter, r *http.Request) {
    deviceKey := chi.URLParam(r, "key")
    tagKey := chi.URLParam(r, "tag_key")
    h.pg.Exec(r.Context(),
        `DELETE FROM device_tags WHERE device_key = $1 AND key = $2`,
        deviceKey, tagKey)
    writeJSON(w, http.StatusOK, map[string]string{"status": "deleted"})
}
```

- [ ] **Step 2: Write `internal/groups_test.go`**

```go
// backend/internal/groups_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "strings"
    "testing"
)

func TestCreateGroup_Validation(t *testing.T) {
    h := &GroupHandler{}
    body := `{"name": ""}`
    req := httptest.NewRequest("POST", "/api/v1/groups", strings.NewReader(body))
    req.Header.Set("Content-Type", "application/json")
    rec := httptest.NewRecorder()
    h.CreateGroup(rec, req)
    if rec.Code != http.StatusBadRequest {
        t.Errorf("status = %d, want 400", rec.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestCreateGroup ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire group routes in cmd/api/main.go**

```go
// Add to the protected group:
r.Get("/groups", groupHandler.ListGroups)
r.Post("/groups", groupHandler.CreateGroup)
r.Post("/groups/{id}/devices/{key}", groupHandler.AddDeviceToGroup)
r.Delete("/groups/{id}/devices/{key}", groupHandler.RemoveDeviceFromGroup)
r.Get("/devices/{key}/tags", groupHandler.ListTags)
r.Post("/devices/{key}/tags/{tag_key}", groupHandler.SetTag)
r.Delete("/devices/{key}/tags/{tag_key}", groupHandler.DeleteTag)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/groups.go backend/internal/groups_test.go
git commit -m "feat: device groups and tags CRUD"
```

---

### Task 6: Search (PostgreSQL full-text search)

**Files:**
- Create: `backend/internal/search.go`
- Create: `backend/internal/search_test.go`

- [ ] **Step 1: Write `internal/search.go`**

```go
// internal/search.go — Full-text search across devices, audit log.
// Uses PostgreSQL tsvector with GIN indexes. No external search service.

package internal

import (
    "net/http"
    "strconv"
    "strings"

    "github.com/jackc/pgx/v5/pgxpool"
)

type SearchHandler struct {
    pg *pgxpool.Pool
}

type SearchResult struct {
    ID         string `json:"id"`
    EntityType string `json:"entity_type"`
    Label      string `json:"label"`
    Subtitle   string `json:"subtitle,omitempty"`
}

func NewSearchHandler(pg *pgxpool.Pool) *SearchHandler {
    return &SearchHandler{pg: pg}
}

func (h *SearchHandler) Search(w http.ResponseWriter, r *http.Request) {
    q := r.URL.Query().Get("q")
    types := r.URL.Query().Get("type") // comma-separated: devices,audit
    limit := parseInt(r.URL.Query().Get("limit"), 20)
    offset := parseInt(r.URL.Query().Get("offset"), 0)

    if q == "" {
        writeError(w, "validation_error", "query q is required", http.StatusBadRequest)
        return
    }

    results := []SearchResult{}

    if types == "" || strings.Contains(types, "devices") {
        rows, _ := h.pg.Query(r.Context(), `
            SELECT device_key, device_name, device_type
            FROM devices
            WHERE search_vector @@ plainto_tsquery('english', $1)
               OR device_key ILIKE '%' || $1 || '%'
            ORDER BY ts_rank(search_vector, plainto_tsquery('english', $1)) DESC
            LIMIT $2 OFFSET $3`, q, limit, offset)
        if rows != nil {
            for rows.Next() {
                var key, name, dtype string
                rows.Scan(&key, &name, &dtype)
                results = append(results, SearchResult{
                    ID: key, EntityType: "device", Label: name, Subtitle: dtype,
                })
            }
            rows.Close()
        }
    }

    if strings.Contains(types, "audit") {
        rows, _ := h.pg.Query(r.Context(), `
            SELECT id::text, action, resource_type
            FROM audit_log
            WHERE search_vector @@ plainto_tsquery('english', $1)
            ORDER BY created_at DESC
            LIMIT $2 OFFSET $3`, q, limit, offset)
        if rows != nil {
            for rows.Next() {
                var id, action, rtype string
                rows.Scan(&id, &action, &rtype)
                results = append(results, SearchResult{
                    ID: id, EntityType: "audit", Label: action, Subtitle: rtype,
                })
            }
            rows.Close()
        }
    }

    writeJSON(w, http.StatusOK, map[string]any{
        "results": results,
        "total":   len(results),
        "query":   q,
    })
}

func parseInt(s string, def int) int {
    if s == "" {
        return def
    }
    v, err := strconv.Atoi(s)
    if err != nil {
        return def
    }
    return v
}
```

- [ ] **Step 2: Write `internal/search_test.go`**

```go
// backend/internal/search_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "testing"
)

func TestSearch_EmptyQuery(t *testing.T) {
    h := &SearchHandler{}
    req := httptest.NewRequest("GET", "/api/v1/search?q=", nil)
    rec := httptest.NewRecorder()
    h.Search(rec, req)
    if rec.Code != http.StatusBadRequest {
        t.Errorf("status = %d, want 400", rec.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestSearch ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire search route in cmd/api/main.go**

```go
// Add to the protected group:
r.Get("/search", searchHandler.Search)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/search.go backend/internal/search_test.go
git commit -m "feat: full-text search across devices and audit log"
```

---

### Task 7: Rate limiting (token bucket)

**Files:**
- Create: `backend/internal/ratelimit.go`
- Create: `backend/internal/ratelimit_test.go`

- [ ] **Step 1: Write `internal/ratelimit.go`**

```go
// internal/ratelimit.go — Token bucket rate limiter.
// Per-IP for auth endpoints, per-token for API endpoints.

package internal

import (
    "net/http"
    "strconv"
    "sync"
    "time"
)

type RateLimiter struct {
    mu       sync.Mutex
    buckets  map[string]*tokenBucket
    maxRate  int
    refill   time.Duration
}

type tokenBucket struct {
    tokens    float64
    lastRefill time.Time
}

func NewRateLimiter(maxRate int, refill time.Duration) *RateLimiter {
    return &RateLimiter{
        buckets: make(map[string]*tokenBucket),
        maxRate: maxRate,
        refill:  refill,
    }
}

func (rl *RateLimiter) Allow(key string) bool {
    rl.mu.Lock()
    defer rl.mu.Unlock()

    b, ok := rl.buckets[key]
    if !ok {
        b = &tokenBucket{tokens: float64(rl.maxRate), lastRefill: time.Now()}
        rl.buckets[key] = b
    }

    // Refill
    elapsed := time.Since(b.lastRefill).Seconds()
    b.tokens += elapsed * (float64(rl.maxRate) / rl.refill.Seconds())
    if b.tokens > float64(rl.maxRate) {
        b.tokens = float64(rl.maxRate)
    }
    b.lastRefill = time.Now()

    if b.tokens >= 1 {
        b.tokens--
        return true
    }
    return false
}

// RateLimitMiddleware limits requests per IP. Use for auth endpoints.
func RateLimitMiddleware(maxRate int, refill time.Duration) func(http.Handler) http.Handler {
    limiter := NewRateLimiter(maxRate, refill)
    return func(next http.Handler) http.Handler {
        return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            key := r.RemoteAddr
            if !limiter.Allow(key) {
                w.Header().Set("Retry-After", strconv.Itoa(int(refill.Seconds())))
                writeError(w, "rate_limited", "too many requests", http.StatusTooManyRequests)
                return
            }
            next.ServeHTTP(w, r)
        })
    }
}
```

- [ ] **Step 2: Write `internal/ratelimit_test.go`**

```go
// backend/internal/ratelimit_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "testing"
)

func TestRateLimiter_AllowsWithinLimit(t *testing.T) {
    rl := NewRateLimiter(5, time.Minute)
    for i := 0; i < 5; i++ {
        if !rl.Allow("test-key") {
            t.Errorf("request %d denied, want allowed", i+1)
        }
    }
}

func TestRateLimiter_BlocksAfterLimit(t *testing.T) {
    rl := NewRateLimiter(2, time.Minute)
    rl.Allow("test-key")
    rl.Allow("test-key")
    if rl.Allow("test-key") {
        t.Error("third request allowed, want denied")
    }
}

func TestRateLimitMiddleware(t *testing.T) {
    handler := RateLimitMiddleware(1, time.Minute)(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        w.WriteHeader(http.StatusOK)
    }))

    req := httptest.NewRequest("GET", "/api/v1/auth/login", nil)
    rec := httptest.NewRecorder()
    handler.ServeHTTP(rec, req)
    if rec.Code != http.StatusOK {
        t.Errorf("first request: status = %d, want 200", rec.Code)
    }

    rec2 := httptest.NewRecorder()
    handler.ServeHTTP(rec2, req)
    if rec2.Code != http.StatusTooManyRequests {
        t.Errorf("second request: status = %d, want 429", rec2.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestRateLimit ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire rate limiting in cmd/api/main.go**

```go
// Apply to auth routes:
r.With(internal.RateLimitMiddleware(10, time.Minute)).Post("/auth/login", h.Login)
r.With(internal.RateLimitMiddleware(5, time.Minute)).Post("/auth/register", h.Register)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/ratelimit.go backend/internal/ratelimit_test.go
git commit -m "feat: token bucket rate limiter for auth endpoints"
```

---

### Task 8: Notification preferences + ClickHouse hourly MV

**Files:**
- Create: `backend/clickhouse/init/002_mv_hourly.sql`

- [ ] **Step 1: Write ClickHouse hourly MV**

```sql
-- clickhouse/init/002_mv_hourly.sql
CREATE MATERIALIZED VIEW IF NOT EXISTS telemetry_hourly
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

- [ ] **Step 2: Add notification preferences handler**

```go
// Add to handlers.go:
func (h *Handlers) GetNotificationPrefs(w http.ResponseWriter, r *http.Request) {
    userID := r.Context().Value(ContextUserID).(string)
    var prefs struct {
        AlertFired    bool `json:"alert_fired_email"`
        AlertResolved bool `json:"alert_resolved_email"`
        QuietStart    *int `json:"quiet_hours_start,omitempty"`
        QuietEnd      *int `json:"quiet_hours_end,omitempty"`
    }
    err := h.pg.QueryRow(r.Context(),
        `SELECT alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end
         FROM notification_preferences WHERE user_id = $1`, userID).
        Scan(&prefs.AlertFired, &prefs.AlertResolved, &prefs.QuietStart, &prefs.QuietEnd)
    if err != nil {
        writeJSON(w, http.StatusOK, prefs) // defaults
        return
    }
    writeJSON(w, http.StatusOK, prefs)
}

func (h *Handlers) UpdateNotificationPrefs(w http.ResponseWriter, r *http.Request) {
    userID := r.Context().Value(ContextUserID).(string)
    var req struct {
        AlertFired    *bool `json:"alert_fired_email"`
        AlertResolved *bool `json:"alert_resolved_email"`
        QuietStart    *int  `json:"quiet_hours_start"`
        QuietEnd      *int  `json:"quiet_hours_end"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
        writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
        return
    }
    h.pg.Exec(r.Context(), `
        INSERT INTO notification_preferences (user_id, alert_fired_email, alert_resolved_email, quiet_hours_start, quiet_hours_end)
        VALUES ($1, $2, $3, $4, $5)
        ON CONFLICT (user_id) DO UPDATE SET
            alert_fired_email = COALESCE($2, notification_preferences.alert_fired_email),
            alert_resolved_email = COALESCE($3, notification_preferences.alert_resolved_email),
            quiet_hours_start = $4,
            quiet_hours_end = $5,
            updated_at = now()`,
        userID, req.AlertFired, req.AlertResolved, req.QuietStart, req.QuietEnd)
    writeJSON(w, http.StatusOK, map[string]string{"status": "updated"})
}
```

- [ ] **Step 3: Wire notification routes**

```go
// Add to protected group:
r.Get("/users/me/notifications", h.GetNotificationPrefs)
r.Patch("/users/me/notifications", h.UpdateNotificationPrefs)
```

- [ ] **Step 4: Commit**

```bash
git add backend/clickhouse/init/002_mv_hourly.sql
git commit -m "feat: ClickHouse hourly MV, notification preferences handler"
```

---

## Phase 2 Summary

| Task | What | Files |
|---|---|---|
| 1 | Migration 002 | 2 SQL files |
| 2 | OTA releases + check endpoint | ota.go, ota_test.go |
| 3 | Alert engine | alerts.go, alerts_test.go |
| 4 | Email service (async queue) | email.go, email_test.go |
| 5 | Device groups/tags CRUD | groups.go, groups_test.go |
| 6 | Full-text search | search.go, search_test.go |
| 7 | Rate limiting | ratelimit.go, ratelimit_test.go |
| 8 | Notification prefs + CH MV | handlers.go, 002_mv_hourly.sql |
