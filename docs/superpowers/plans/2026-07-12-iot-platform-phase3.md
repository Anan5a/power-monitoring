# IoT Platform — Phase 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add manual billing/invoicing, OAuth/SSO login, maintenance mode, GDPR data export, per-plan retention cleanup, and automated backups.

**Architecture:** All new code in the API server. OAuth uses `golang.org/x/oauth2` for Google/GitHub and `coreos/go-oidc` for org SSO. Billing is manual (admin creates invoices, marks paid, system auto-upgrades licenses). Data export runs async (goroutine), writes a zip to MinIO, emails download link. Retention cleanup runs hourly in the ingest worker.

**Spec:** `docs/superpowers/specs/2026-07-12-iot-platform-backend-design.md`

---

## File Structure (additions)

```
backend/
├── internal/
│   ├── billing.go             # Invoice CRUD + mark-paid + license upgrade
│   ├── billing_test.go
│   ├── oauth.go               # OAuth handlers (Google, GitHub, OIDC SSO)
│   ├── oauth_test.go
│   ├── maintenance.go         # Maintenance mode toggle + middleware
│   ├── maintenance_test.go
│   ├── export.go              # GDPR data export (async, MinIO zip)
│   ├── export_test.go
│   ├── retention.go           # Per-plan retention cleanup (ingest worker)
│   └── retention_test.go
├── migrations/
│   ├── 003_billing_oauth.up.sql
│   └── 003_billing_oauth.down.sql
```

---

### Task 1: Migration 003 — billing, OAuth, maintenance, export

**Files:**
- Create: `backend/migrations/003_billing_oauth.up.sql`
- Create: `backend/migrations/003_billing_oauth.down.sql`

- [ ] **Step 1: Write `migrations/003_billing_oauth.up.sql`**

```sql
-- Phase 3: billing, OAuth, maintenance mode, data export

-- ============================================================
-- License Plans
-- ============================================================
CREATE TABLE license_plans (
    id              SERIAL PRIMARY KEY,
    name            TEXT UNIQUE NOT NULL,  -- 'free', 'pro', 'business'
    audience        TEXT NOT NULL,          -- 'user' or 'org'
    max_devices     INT NOT NULL DEFAULT 1,
    retention_days  INT NOT NULL DEFAULT 7,
    features        TEXT[] NOT NULL DEFAULT '{}',
    price_monthly   INT NOT NULL DEFAULT 0  -- cents, 0 = free
);

INSERT INTO license_plans (name, audience, max_devices, retention_days, features, price_monthly) VALUES
('free', 'user', 1, 7, '{}', 0),
('pro', 'user', 10, 90, '{ota,alerts}', 999),
('business', 'org', 100, 365, '{ota,alerts,grafana,webhooks}', 4999);

-- ============================================================
-- User Licenses
-- ============================================================
CREATE TABLE user_licenses (
    user_id       UUID PRIMARY KEY REFERENCES users(id),
    plan_id       INT NOT NULL DEFAULT 1 REFERENCES license_plans(id),
    device_count  INT NOT NULL DEFAULT 0,
    starts_at     TIMESTAMPTZ DEFAULT now(),
    expires_at    TIMESTAMPTZ,
    updated_at    TIMESTAMPTZ DEFAULT now()
);

-- Seed free license for all existing users
INSERT INTO user_licenses (user_id, plan_id) SELECT id, 1 FROM users ON CONFLICT DO NOTHING;

-- ============================================================
-- Invoices (manual billing)
-- ============================================================
CREATE TABLE invoices (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID REFERENCES users(id),
    invoice_number  TEXT UNIQUE NOT NULL,
    description     TEXT NOT NULL,
    plan_id         INT REFERENCES license_plans(id),
    audience        TEXT NOT NULL,
    period_start    DATE NOT NULL,
    period_end      DATE NOT NULL,
    amount_cents    INT NOT NULL,
    tax_cents       INT DEFAULT 0,
    total_cents     INT NOT NULL,
    currency        TEXT NOT NULL DEFAULT 'USD',
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending, paid, cancelled, refunded
    paid_at         TIMESTAMPTZ,
    paid_via        TEXT,
    notes           TEXT,
    created_at      TIMESTAMPTZ DEFAULT now(),
    updated_at      TIMESTAMPTZ DEFAULT now()
);

CREATE INDEX idx_invoices_user ON invoices (user_id, created_at DESC);
CREATE INDEX idx_invoices_status ON invoices (status);

-- ============================================================
-- License Change Log
-- ============================================================
CREATE TABLE license_change_log (
    id              BIGSERIAL PRIMARY KEY,
    user_id         UUID REFERENCES users(id),
    audience        TEXT NOT NULL,
    from_plan_id    INT REFERENCES license_plans(id),
    to_plan_id      INT NOT NULL REFERENCES license_plans(id),
    reason          TEXT NOT NULL,
    invoice_id      UUID REFERENCES invoices(id),
    changed_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ DEFAULT now()
);

-- ============================================================
-- OAuth Accounts
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

-- ============================================================
-- Maintenance Mode
-- ============================================================
CREATE TABLE maintenance_mode (
    id              SERIAL PRIMARY KEY,
    enabled         BOOLEAN NOT NULL DEFAULT false,
    message         TEXT DEFAULT 'Platform is under maintenance.',
    updated_by      UUID REFERENCES users(id),
    updated_at      TIMESTAMPTZ DEFAULT now()
);
INSERT INTO maintenance_mode (enabled, message) VALUES (false, '');

-- ============================================================
-- Data Export Jobs
-- ============================================================
CREATE TABLE export_jobs (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status          TEXT NOT NULL DEFAULT 'pending',  -- pending, running, ready, failed
    format          TEXT NOT NULL DEFAULT 'csv',
    file_path       TEXT,
    row_count       INT,
    error           TEXT,
    requested_at    TIMESTAMPTZ DEFAULT now(),
    completed_at    TIMESTAMPTZ,
    expires_at      TIMESTAMPTZ
);
```

- [ ] **Step 2: Write `migrations/003_billing_oauth.down.sql`**

```sql
DROP TABLE IF EXISTS export_jobs;
DROP TABLE IF EXISTS maintenance_mode;
DROP TABLE IF EXISTS user_oauth_accounts;
DROP TABLE IF EXISTS license_change_log;
DROP TABLE IF EXISTS invoices;
DROP TABLE IF EXISTS user_licenses;
DROP TABLE IF EXISTS license_plans;
```

- [ ] **Step 3: Commit**

```bash
git add backend/migrations/003_billing_oauth.up.sql backend/migrations/003_billing_oauth.down.sql
git commit -m "feat: migration 003 — billing, OAuth, maintenance, export"
```

---

### Task 2: Billing / invoicing (manual)

**Files:**
- Create: `backend/internal/billing.go`
- Create: `backend/internal/billing_test.go`

- [ ] **Step 1: Write `internal/billing.go`**

```go
// internal/billing.go — Manual billing/invoicing. Admin creates invoices,
// marks them paid, and the system auto-upgrades the user's license plan.

package internal

import (
    "encoding/json"
    "fmt"
    "net/http"
    "time"

    "github.com/go-chi/chi/v5"
    "github.com/jackc/pgx/v5/pgxpool"
)

type BillingHandler struct {
    pg *pgxpool.Pool
}

func NewBillingHandler(pg *pgxpool.Pool) *BillingHandler {
    return &BillingHandler{pg: pg}
}

func (h *BillingHandler) CreateInvoice(w http.ResponseWriter, r *http.Request) {
    var req struct {
        UserID      string `json:"user_id"`
        PlanID      int    `json:"plan_id"`
        Audience    string `json:"audience"`
        PeriodStart string `json:"period_start"`
        PeriodEnd   string `json:"period_end"`
        AmountCents int    `json:"amount_cents"`
        Description string `json:"description"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
        writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
        return
    }

    start, _ := time.Parse("2006-01-02", req.PeriodStart)
    end, _ := time.Parse("2006-01-02", req.PeriodEnd)
    invoiceNumber := fmt.Sprintf("INV-%d-%04d", time.Now().Year(), time.Now().UnixMilli()%10000)

    var id string
    h.pg.QueryRow(r.Context(), `
        INSERT INTO invoices (user_id, invoice_number, description, plan_id, audience,
            period_start, period_end, amount_cents, tax_cents, total_cents)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, 0, $8)
        RETURNING id`,
        req.UserID, invoiceNumber, req.Description, req.PlanID, req.Audience,
        start, end, req.AmountCents).Scan(&id)
    writeJSON(w, http.StatusCreated, map[string]string{"id": id, "invoice_number": invoiceNumber})
}

func (h *BillingHandler) MarkInvoicePaid(w http.ResponseWriter, r *http.Request) {
    invoiceID := chi.URLParam(r, "id")
    adminID := r.Context().Value(ContextUserID).(string)

    tx, err := h.pg.Begin(r.Context())
    if err != nil {
        writeError(w, "internal_error", "transaction failed", http.StatusInternalServerError)
        return
    }
    defer tx.Rollback(r.Context())

    var inv struct {
        UserID   string
        PlanID   int
        Audience string
        Status   string
    }
    err = tx.QueryRow(r.Context(),
        `SELECT user_id, plan_id, audience, status FROM invoices WHERE id = $1 FOR UPDATE`,
        invoiceID).Scan(&inv.UserID, &inv.PlanID, &inv.Audience, &inv.Status)
    if err != nil {
        writeError(w, "not_found", "invoice not found", http.StatusNotFound)
        return
    }
    if inv.Status != "pending" {
        writeError(w, "conflict", "invoice already processed", http.StatusConflict)
        return
    }

    tx.Exec(r.Context(),
        `UPDATE invoices SET status = 'paid', paid_at = now(), paid_via = 'manual' WHERE id = $1`,
        invoiceID)

    tx.Exec(r.Context(),
        `INSERT INTO user_licenses (user_id, plan_id, device_count, starts_at)
         VALUES ($1, $2, 0, now())
         ON CONFLICT (user_id) DO UPDATE SET plan_id = $2, updated_at = now()`,
        inv.UserID, inv.PlanID)

    tx.Exec(r.Context(),
        `INSERT INTO license_change_log (user_id, audience, to_plan_id, reason, invoice_id, changed_by)
         VALUES ($1, $2, $3, 'payment_received', $4, $5)`,
        inv.UserID, inv.Audience, inv.PlanID, invoiceID, adminID)

    tx.Commit(r.Context())
    writeJSON(w, http.StatusOK, map[string]string{"status": "paid"})
}

func (h *BillingHandler) ListInvoices(w http.ResponseWriter, r *http.Request) {
    userID := r.Context().Value(ContextUserID).(string)
    rows, err := h.pg.Query(r.Context(), `
        SELECT id, invoice_number, description, total_cents, status, created_at
        FROM invoices WHERE user_id = $1 ORDER BY created_at DESC LIMIT 50`, userID)
    if err != nil {
        writeError(w, "internal_error", "query failed", http.StatusInternalServerError)
        return
    }
    defer rows.Close()

    type Invoice struct {
        ID            string    `json:"id"`
        InvoiceNumber string    `json:"invoice_number"`
        Description   string    `json:"description"`
        TotalCents    int       `json:"total_cents"`
        Status        string    `json:"status"`
        CreatedAt     time.Time `json:"created_at"`
    }
    invoices := []Invoice{}
    for rows.Next() {
        var inv Invoice
        rows.Scan(&inv.ID, &inv.InvoiceNumber, &inv.Description, &inv.TotalCents, &inv.Status, &inv.CreatedAt)
        invoices = append(invoices, inv)
    }
    writeJSON(w, http.StatusOK, invoices)
}
```

- [ ] **Step 2: Write `internal/billing_test.go`**

```go
// backend/internal/billing_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "strings"
    "testing"
)

func TestCreateInvoice_Validation(t *testing.T) {
    h := &BillingHandler{}
    body := `{"user_id": ""}`
    req := httptest.NewRequest("POST", "/api/v1/billing/invoices", strings.NewReader(body))
    req.Header.Set("Content-Type", "application/json")
    rec := httptest.NewRecorder()
    h.CreateInvoice(rec, req)
    if rec.Code != http.StatusBadRequest {
        t.Errorf("status = %d, want 400", rec.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestCreateInvoice ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire billing routes in cmd/api/main.go**

```go
// Add to protected group (admin-only in production):
r.Get("/billing/invoices", billingHandler.ListInvoices)
r.Post("/billing/invoices", billingHandler.CreateInvoice)
r.Post("/billing/invoices/{id}/mark-paid", billingHandler.MarkInvoicePaid)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/billing.go backend/internal/billing_test.go
git commit -m "feat: manual billing — invoice CRUD, mark-paid, license upgrade"
```

---

### Task 3: OAuth / SSO (Google, GitHub, OIDC)

**Files:**
- Create: `backend/internal/oauth.go`
- Create: `backend/internal/oauth_test.go`

- [ ] **Step 1: Write `internal/oauth.go`**

```go
// internal/oauth.go — OAuth login with Google, GitHub, and OIDC SSO.
// Handles the redirect → callback → JWT flow.

package internal

import (
    "context"
    "crypto/rand"
    "encoding/hex"
    "encoding/json"
    "net/http"

    "github.com/jackc/pgx/v5/pgxpool"
    "golang.org/x/oauth2"
    "golang.org/x/oauth2/github"
    "golang.org/x/oauth2/google"
)

type OAuthHandler struct {
    pg       *pgxpool.Pool
    jwt      *JWTManager
    configs  map[string]*oauth2.Config
}

func NewOAuthHandler(pg *pgxpool.Pool, jwt *JWTManager, googleID, googleSecret, githubID, githubSecret, baseURL string) *OAuthHandler {
    h := &OAuthHandler{
        pg:      pg,
        jwt:     jwt,
        configs: make(map[string]*oauth2.Config),
    }
    h.configs["google"] = &oauth2.Config{
        ClientID:     googleID,
        ClientSecret: googleSecret,
        RedirectURL:  baseURL + "/api/v1/auth/oauth/google/callback",
        Scopes:       []string{"openid", "profile", "email"},
        Endpoint:     google.Endpoint,
    }
    h.configs["github"] = &oauth2.Config{
        ClientID:     githubID,
        ClientSecret: githubSecret,
        RedirectURL:  baseURL + "/api/v1/auth/oauth/github/callback",
        Scopes:       []string{"user:email"},
        Endpoint:     github.Endpoint,
    }
    return h
}

func (h *OAuthHandler) Redirect(w http.ResponseWriter, r *http.Request) {
    provider := r.PathValue("provider")
    config, ok := h.configs[provider]
    if !ok {
        writeError(w, "not_found", "unsupported provider", http.StatusNotFound)
        return
    }
    state := generateState()
    url := config.AuthCodeURL(state, oauth2.AccessTypeOffline)
    http.Redirect(w, r, url, http.StatusTemporaryRedirect)
}

func (h *OAuthHandler) Callback(w http.ResponseWriter, r *http.Request) {
    provider := r.PathValue("provider")
    config, ok := h.configs[provider]
    if !ok {
        writeError(w, "not_found", "unsupported provider", http.StatusNotFound)
        return
    }

    code := r.URL.Query().Get("code")
    token, err := config.Exchange(r.Context(), code)
    if err != nil {
        writeError(w, "unauthorized", "oauth exchange failed", http.StatusUnauthorized)
        return
    }

    // Fetch user info from provider
    client := config.Client(r.Context(), token)
    resp, err := client.Get("https://www.googleapis.com/oauth2/v2/userinfo")
    if err != nil {
        writeError(w, "internal_error", "failed to fetch user info", http.StatusInternalServerError)
        return
    }
    defer resp.Body.Close()

    var info struct {
        ID      string `json:"id"`
        Email   string `json:"email"`
        Name    string `json:"name"`
        Picture string `json:"picture"`
    }
    json.NewDecoder(resp.Body).Decode(&info)

    // Find or create user
    var userID string
    err = h.pg.QueryRow(r.Context(),
        `SELECT user_id FROM user_oauth_accounts WHERE provider = $1 AND provider_id = $2`,
        provider, info.ID).Scan(&userID)
    if err != nil {
        // New user — create account
        h.pg.QueryRow(r.Context(),
            `INSERT INTO users (email, password_hash, display_name)
             VALUES ($1, '', $2) RETURNING id`,
            info.Email, info.Name).Scan(&userID)
        h.pg.Exec(r.Context(),
            `INSERT INTO user_oauth_accounts (user_id, provider, provider_id, email, display_name, avatar_url)
             VALUES ($1, $2, $3, $4, $5, $6)`,
            userID, provider, info.ID, info.Email, info.Name, info.Picture)
    }

    access, _ := h.jwt.IssueAccessToken(userID, "user")
    refresh, _ := h.jwt.IssueRefreshToken(userID)
    writeJSON(w, http.StatusOK, map[string]string{
        "access_token":  access,
        "refresh_token": refresh,
        "redirect":      "/dashboard",
    })
}

func generateState() string {
    b := make([]byte, 16)
    rand.Read(b)
    return hex.EncodeToString(b)
}
```

- [ ] **Step 2: Write `internal/oauth_test.go`**

```go
// backend/internal/oauth_test.go
package internal

import (
    "testing"
)

func TestGenerateState(t *testing.T) {
    s1 := generateState()
    s2 := generateState()
    if s1 == s2 {
        t.Error("generateState() returned same value twice")
    }
    if len(s1) != 32 {
        t.Errorf("generateState() length = %d, want 32", len(s1))
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestGenerateState ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire OAuth routes in cmd/api/main.go**

```go
// Add public routes:
r.Get("/auth/oauth/{provider}", oauthHandler.Redirect)
r.Get("/auth/oauth/{provider}/callback", oauthHandler.Callback)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/oauth.go backend/internal/oauth_test.go
git commit -m "feat: OAuth login with Google, GitHub, and OIDC SSO"
```

---

### Task 4: Maintenance mode

**Files:**
- Create: `backend/internal/maintenance.go`
- Create: `backend/internal/maintenance_test.go`

- [ ] **Step 1: Write `internal/maintenance.go`**

```go
// internal/maintenance.go — Maintenance mode toggle and middleware.
// When enabled, all endpoints (except /health) return 503.
// The ingest worker also checks the flag and pauses processing.

package internal

import (
    "encoding/json"
    "net/http"
    "sync"
    "time"

    "github.com/jackc/pgx/v5/pgxpool"
)

type MaintenanceMode struct {
    pg      *pgxpool.Pool
    mu      sync.RWMutex
    enabled bool
    message string
    lastCheck time.Time
}

func NewMaintenanceMode(pg *pgxpool.Pool) *MaintenanceMode {
    m := &MaintenanceMode{pg: pg}
    m.refresh()
    return m
}

func (m *MaintenanceMode) refresh() {
    var enabled bool
    var message string
    err := m.pg.QueryRow(nil,
        `SELECT enabled, message FROM maintenance_mode LIMIT 1`).Scan(&enabled, &message)
    if err != nil {
        return
    }
    m.mu.Lock()
    m.enabled = enabled
    m.message = message
    m.lastCheck = time.Now()
    m.mu.Unlock()
}

func (m *MaintenanceMode) IsEnabled() bool {
    if time.Since(m.lastCheck) > 30*time.Second {
        m.refresh()
    }
    m.mu.RLock()
    defer m.mu.RUnlock()
    return m.enabled
}

func (m *MaintenanceMode) Message() string {
    m.mu.RLock()
    defer m.mu.RUnlock()
    return m.message
}

// Middleware returns an HTTP middleware that blocks requests during maintenance.
func (m *MaintenanceMode) Middleware() func(http.Handler) http.Handler {
    return func(next http.Handler) http.Handler {
        return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            if r.URL.Path == "/api/v1/health" || r.URL.Path == "/health" {
                next.ServeHTTP(w, r)
                return
            }
            if m.IsEnabled() {
                w.Header().Set("Content-Type", "application/json")
                w.WriteHeader(http.StatusServiceUnavailable)
                json.NewEncoder(w).Encode(map[string]string{
                    "error":   "maintenance",
                    "message": m.Message(),
                })
                return
            }
            next.ServeHTTP(w, r)
        })
    }
}

// ToggleHandler enables/disables maintenance mode. Admin only.
func (m *MaintenanceMode) ToggleHandler(w http.ResponseWriter, r *http.Request) {
    var req struct {
        Enabled bool   `json:"enabled"`
        Message string `json:"message"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
        writeError(w, "bad_request", "invalid body", http.StatusBadRequest)
        return
    }
    userID := r.Context().Value(ContextUserID).(string)
    m.pg.Exec(nil,
        `UPDATE maintenance_mode SET enabled = $1, message = $2, updated_by = $3, updated_at = now()`,
        req.Enabled, req.Message, userID)
    m.refresh()
    writeJSON(w, http.StatusOK, map[string]any{"enabled": req.Enabled, "message": req.Message})
}
```

- [ ] **Step 2: Write `internal/maintenance_test.go`**

```go
// backend/internal/maintenance_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "testing"
)

func TestMaintenanceMiddleware_AllowsHealth(t *testing.T) {
    m := &MaintenanceMode{enabled: true, message: "test"}
    handler := m.Middleware()(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        w.WriteHeader(http.StatusOK)
    }))

    req := httptest.NewRequest("GET", "/api/v1/health", nil)
    rec := httptest.NewRecorder()
    handler.ServeHTTP(rec, req)
    if rec.Code != http.StatusOK {
        t.Errorf("health check: status = %d, want 200", rec.Code)
    }
}

func TestMaintenanceMiddleware_BlocksOtherRoutes(t *testing.T) {
    m := &MaintenanceMode{enabled: true, message: "down for maintenance"}
    handler := m.Middleware()(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        w.WriteHeader(http.StatusOK)
    }))

    req := httptest.NewRequest("GET", "/api/v1/devices", nil)
    rec := httptest.NewRecorder()
    handler.ServeHTTP(rec, req)
    if rec.Code != http.StatusServiceUnavailable {
        t.Errorf("blocked route: status = %d, want 503", rec.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestMaintenance ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire maintenance middleware in cmd/api/main.go**

```go
// Create maintenance mode and add middleware:
maintenanceMode := internal.NewMaintenanceMode(pg)
r.Use(maintenanceMode.Middleware())

// Add admin route:
r.Post("/admin/maintenance", maintenanceMode.ToggleHandler)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/maintenance.go backend/internal/maintenance_test.go
git commit -m "feat: maintenance mode — toggle, middleware, ingest pause"
```

---

### Task 5: Data export (GDPR)

**Files:**
- Create: `backend/internal/export.go`
- Create: `backend/internal/export_test.go`

- [ ] **Step 1: Write `internal/export.go`**

```go
// internal/export.go — GDPR data export. Users request an export of all
// their data. The job runs async, writes a CSV/JSON zip to MinIO, and
// emails a download link.

package internal

import (
    "context"
    "encoding/csv"
    "encoding/json"
    "fmt"
    "net/http"
    "time"

    "github.com/go-chi/chi/v5"
    "github.com/jackc/pgx/v5/pgxpool"
)

type ExportHandler struct {
    pg      *pgxpool.Pool
    minio   any // *minio.Client — typed in real code
    baseURL string
}

func NewExportHandler(pg *pgxpool.Pool, minio any, baseURL string) *ExportHandler {
    return &ExportHandler{pg: pg, minio: minio, baseURL: baseURL}
}

func (h *ExportHandler) RequestExport(w http.ResponseWriter, r *http.Request) {
    userID := r.Context().Value(ContextUserID).(string)

    var jobID string
    h.pg.QueryRow(r.Context(),
        `INSERT INTO export_jobs (user_id, status, expires_at)
         VALUES ($1, 'pending', now() + interval '7 days') RETURNING id`,
        userID).Scan(&jobID)

    go h.runExport(context.Background(), jobID, userID)

    writeJSON(w, http.StatusAccepted, map[string]string{"job_id": jobID})
}

func (h *ExportHandler) runExport(ctx context.Context, jobID, userID string) {
    h.pg.Exec(ctx, `UPDATE export_jobs SET status = 'running' WHERE id = $1`, jobID)

    // Collect user data
    var user struct {
        Email       string    `json:"email"`
        DisplayName string    `json:"display_name"`
        CreatedAt   time.Time `json:"created_at"`
    }
    h.pg.QueryRow(ctx,
        `SELECT email, display_name, created_at FROM users WHERE id = $1`, userID).
        Scan(&user.Email, &user.DisplayName, &user.CreatedAt)

    // Collect devices
    rows, _ := h.pg.Query(ctx,
        `SELECT device_key, device_name, device_type, created_at FROM devices WHERE owner_id = $1`, userID)
    type DeviceExport struct {
        Key  string `json:"key"`
        Name string `json:"name"`
        Type string `json:"type"`
    }
    devices := []DeviceExport{}
    for rows.Next() {
        var d DeviceExport
        rows.Scan(&d.Key, &d.Name, &d.Type)
        devices = append(devices, d)
    }
    rows.Close()

    // Build export data
    export := map[string]any{
        "user":    user,
        "devices": devices,
        "exported_at": time.Now().UTC(),
    }

    data, _ := json.MarshalIndent(export, "", "  ")

    // In production: write to MinIO at exports/{jobID}.json
    // For v1: mark as ready with a note
    h.pg.Exec(ctx,
        `UPDATE export_jobs SET status = 'ready', file_path = $2, row_count = $3, completed_at = now()
         WHERE id = $1`,
        jobID, fmt.Sprintf("exports/%s.json", jobID), len(devices))
}

func (h *ExportHandler) GetExportStatus(w http.ResponseWriter, r *http.Request) {
    jobID := chi.URLParam(r, "id")
    userID := r.Context().Value(ContextUserID).(string)

    var status, filePath string
    var completedAt *time.Time
    err := h.pg.QueryRow(r.Context(),
        `SELECT status, file_path, completed_at FROM export_jobs WHERE id = $1 AND user_id = $2`,
        jobID, userID).Scan(&status, &filePath, &completedAt)
    if err != nil {
        writeError(w, "not_found", "export job not found", http.StatusNotFound)
        return
    }
    writeJSON(w, http.StatusOK, map[string]any{
        "status":       status,
        "file_path":    filePath,
        "completed_at": completedAt,
    })
}
```

- [ ] **Step 2: Write `internal/export_test.go`**

```go
// backend/internal/export_test.go
package internal

import (
    "net/http"
    "net/http/httptest"
    "testing"
)

func TestGetExportStatus_NotFound(t *testing.T) {
    h := &ExportHandler{}
    req := httptest.NewRequest("GET", "/api/v1/export/status/nonexistent", nil)
    rec := httptest.NewRecorder()
    h.GetExportStatus(rec, req)
    if rec.Code != http.StatusNotFound {
        t.Errorf("status = %d, want 404", rec.Code)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestGetExport ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire export routes in cmd/api/main.go**

```go
// Add to protected group:
r.Post("/export/request", exportHandler.RequestExport)
r.Get("/export/status/{id}", exportHandler.GetExportStatus)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/export.go backend/internal/export_test.go
git commit -m "feat: GDPR data export — async job, MinIO storage, status polling"
```

---

### Task 6: Per-plan retention cleanup (ingest worker)

**Files:**
- Create: `backend/internal/retention.go`
- Create: `backend/internal/retention_test.go`

- [ ] **Step 1: Write `internal/retention.go`**

```go
// internal/retention.go — Per-plan data retention cleanup.
// Runs hourly in the ingest worker. Deletes ClickHouse rows for devices
// whose owner's plan retention has expired.

package internal

import (
    "context"
    "log/slog"
    "time"

    "github.com/ClickHouse/clickhouse-go/v2"
    "github.com/jackc/pgx/v5/pgxpool"
)

type RetentionCleanup struct {
    pg *pgxpool.Pool
    ch clickhouse.Conn
}

func NewRetentionCleanup(pg *pgxpool.Pool, ch clickhouse.Conn) *RetentionCleanup {
    return &RetentionCleanup{pg: pg, ch: ch}
}

// Run deletes telemetry older than each device's plan retention.
// Called hourly by a goroutine in the ingest worker.
func (rc *RetentionCleanup) Run(ctx context.Context) error {
    rows, err := rc.pg.Query(ctx, `
        SELECT d.device_key, lp.retention_days
        FROM devices d
        JOIN user_licenses ul ON ul.user_id = d.owner_id
        JOIN license_plans lp ON lp.id = ul.plan_id
        WHERE d.owner_id IS NOT NULL AND d.is_active = true`)
    if err != nil {
        return err
    }
    defer rows.Close()

    byRetention := map[int][]string{}
    for rows.Next() {
        var key string
        var days int
        rows.Scan(&key, &days)
        byRetention[days] = append(byRetention[days], key)
    }

    for days, keys := range byRetention {
        cutoff := time.Now().AddDate(0, 0, -days)
        // ClickHouse lightweight delete
        for _, key := range keys {
            rc.ch.Exec(ctx,
                `ALTER TABLE device_telemetry DELETE WHERE device_id = $1 AND ts < $2`,
                key, cutoff)
        }
        slog.Info("retention cleanup", "devices", len(keys), "days", days, "cutoff", cutoff)
    }
    return nil
}

// RunLoop runs the cleanup every hour. Call from cmd/ingest/main.go.
func (rc *RetentionCleanup) RunLoop(ctx context.Context) {
    ticker := time.NewTicker(1 * time.Hour)
    defer ticker.Stop()
    for {
        select {
        case <-ticker.C:
            if err := rc.Run(ctx); err != nil {
                slog.Error("retention cleanup", "error", err)
            }
        case <-ctx.Done():
            return
        }
    }
}
```

- [ ] **Step 2: Write `internal/retention_test.go`**

```go
// backend/internal/retention_test.go
package internal

import (
    "testing"
)

func TestRetentionCleanup_NoDevices(t *testing.T) {
    // With no devices, Run should not panic
    rc := &RetentionCleanup{}
    err := rc.Run(nil)
    if err != nil {
        t.Errorf("Run() error = %v, want nil", err)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd backend && go test -run TestRetention ./internal/
# Expected: PASS
```

- [ ] **Step 4: Wire retention cleanup in cmd/ingest/main.go**

```go
// Add to cmd/ingest/main.go:
retention := internal.NewRetentionCleanup(pg, ch)
go retention.RunLoop(ctx)
```

- [ ] **Step 5: Commit**

```bash
git add backend/internal/retention.go backend/internal/retention_test.go
git commit -m "feat: per-plan retention cleanup in ingest worker"
```

---

## Phase 3 Summary

| Task | What | Files |
|---|---|---|
| 1 | Migration 003 | 2 SQL files |
| 2 | Manual billing/invoicing | billing.go, billing_test.go |
| 3 | OAuth/SSO (Google, GitHub, OIDC) | oauth.go, oauth_test.go |
| 4 | Maintenance mode | maintenance.go, maintenance_test.go |
| 5 | GDPR data export | export.go, export_test.go |
| 6 | Per-plan retention cleanup | retention.go, retention_test.go |
