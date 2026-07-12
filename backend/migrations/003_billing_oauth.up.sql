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
