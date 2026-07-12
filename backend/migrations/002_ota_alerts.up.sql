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
