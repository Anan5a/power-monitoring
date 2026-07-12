-- Phase 1: core tables for auth, devices, and telemetry metadata.
-- Full schema (orgs, alerts, billing, etc.) added in later migrations.

CREATE EXTENSION IF NOT EXISTS pgcrypto;

-- ============================================================
-- Users
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
-- Devices
-- ============================================================
CREATE TABLE devices (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_key    TEXT UNIQUE NOT NULL,
    device_name   TEXT NOT NULL DEFAULT 'Unnamed Device',
    device_type   TEXT NOT NULL,
    owner_id      UUID REFERENCES users(id),
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
    status        TEXT NOT NULL DEFAULT 'pending',
    result        JSONB,
    error         TEXT,
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
-- Audit Log
-- ============================================================
CREATE TABLE audit_log (
    id            BIGSERIAL PRIMARY KEY,
    actor_id      UUID REFERENCES users(id),
    actor_type    TEXT NOT NULL,
    action        TEXT NOT NULL,
    resource_type TEXT NOT NULL,
    resource_id   TEXT,
    details       JSONB,
    ip_address    INET,
    user_agent    TEXT,
    created_at    TIMESTAMPTZ DEFAULT now()
);

CREATE INDEX idx_audit_actor ON audit_log (actor_id, created_at DESC);
CREATE INDEX idx_audit_action ON audit_log (action, created_at DESC);
