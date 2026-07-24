-- 005: Server-side refresh-token storage for rotation and revocation.
-- Refresh tokens are JWTs carrying a jti and a family_id; this table records
-- each issued refresh token so the server can rotate on use and detect reuse
-- (a revoked/reused token presented again ⇒ revoke the whole family).

CREATE TABLE refresh_tokens (
    jti         TEXT PRIMARY KEY,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    family_id   UUID NOT NULL,
    expires_at  TIMESTAMPTZ NOT NULL,
    revoked     BOOLEAN NOT NULL DEFAULT FALSE,
    used_at     TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_refresh_tokens_family ON refresh_tokens(family_id);
CREATE INDEX idx_refresh_tokens_user   ON refresh_tokens(user_id);