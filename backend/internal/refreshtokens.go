// internal/refreshtokens.go — Server-side refresh-token storage enabling
// rotation and reuse detection. Each issued refresh token carries a jti and a
// family_id; the store records every token so it can revoke on rotation and
// revoke an entire family when a revoked/reused token is presented again.

package internal

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Refresh-token rotation errors.
var (
	// ErrRefreshTokenReuse is returned when a refresh token that was already
	// rotated or revoked is presented again. Reuse is treated as an attack
	// signal and causes the entire token family to be revoked.
	ErrRefreshTokenReuse = errors.New("refresh token reuse detected")
	// ErrRefreshTokenNotFound is returned when a presented refresh token's jti
	// is not present in the store (e.g. evicted, never issued, or pre-existing
	// the table). The caller should treat this as "log in again".
	ErrRefreshTokenNotFound = errors.New("refresh token not recognized")
)

// RefreshTokenStore persists refresh tokens and handles rotation/reuse detection.
type RefreshTokenStore struct {
	pg  *pgxpool.Pool
	jwt *JWTManager
	ttl time.Duration
}

// NewRefreshTokenStore returns a RefreshTokenStore backed by the given pool.
// The jwt manager is used to sign new refresh tokens; ttl is the per-token
// lifetime stored in the refresh_tokens table.
func NewRefreshTokenStore(pg *pgxpool.Pool, jwt *JWTManager, ttl time.Duration) *RefreshTokenStore {
	return &RefreshTokenStore{pg: pg, jwt: jwt, ttl: ttl}
}

// Issue creates a new refresh token for a fresh login, starting a new family.
func (s *RefreshTokenStore) Issue(ctx context.Context, userID string) (string, error) {
	if s.pg == nil {
		// Refuse rather than issuing an un-revocable token; without the store
		// we cannot detect reuse or rotate, so login is disabled.
		return "", fmt.Errorf("database unavailable")
	}
	jti, err := randomID()
	if err != nil {
		return "", err
	}
	familyID, err := randomID()
	if err != nil {
		return "", err
	}
	token, err := s.jwt.IssueRefreshToken(userID, jti, familyID)
	if err != nil {
		return "", err
	}
	if _, err := s.pg.Exec(ctx,
		`INSERT INTO refresh_tokens (jti, user_id, family_id, expires_at)
		 VALUES ($1, $2, $3, now() + make_interval(secs => $4))`,
		jti, userID, familyID, int(s.ttl.Seconds())); err != nil {
		return "", fmt.Errorf("store refresh token: %w", err)
	}
	return token, nil
}

// Rotate validates a presented refresh token, revokes it, and issues a new
// token in the same family. If the presented token was already revoked/used
// (a reuse attack) the entire family is revoked and ErrRefreshTokenReuse is
// returned. Returns the userID and the new refresh token.
func (s *RefreshTokenStore) Rotate(ctx context.Context, token string) (string, string, error) {
	if s.pg == nil {
		return "", "", fmt.Errorf("database unavailable")
	}
	claims, err := s.jwt.ValidateToken(token)
	if err != nil {
		return "", "", err
	}
	jti := claims.ID
	if jti == "" || claims.UserID == "" {
		return "", "", ErrRefreshTokenNotFound
	}

	var (
		userID   string
		familyID string
		revoked  bool
		used     *time.Time
	)
	err = s.pg.QueryRow(ctx,
		`SELECT user_id::text, family_id::text, revoked, used_at
		 FROM refresh_tokens WHERE jti = $1`, jti).Scan(&userID, &familyID, &revoked, &used)
	if err == pgx.ErrNoRows {
		// Not in the store. If it carries a family, revoke that family — the
		// token may have been issued before this table existed or was evicted,
		// but if family_id is present we treat it as potential reuse: an
		// attacker who stole a valid-looking token should not be able to keep
		// using the rest of the family even if this single jti is unknown.
		if claims.FamilyID != "" {
			s.revokeFamily(ctx, claims.FamilyID)
		}
		return "", "", ErrRefreshTokenNotFound
	}
	if err != nil {
		return "", "", fmt.Errorf("lookup refresh token: %w", err)
	}

	// Reuse: a revoked or already-used token is being presented again. Revoke
	// the whole family so every descendant token is invalidated.
	if revoked || used != nil {
		s.revokeFamily(ctx, familyID)
		return "", "", ErrRefreshTokenReuse
	}

	// Mark the presented token as used/revoked so it cannot be reused. We set
	// both revoked and used_at because Rotate checks either flag — keeping
	// them in sync makes the reuse check deterministic regardless of which
	// column a future query reads.
	if _, err := s.pg.Exec(ctx,
		`UPDATE refresh_tokens SET revoked = true, used_at = now() WHERE jti = $1`, jti); err != nil {
		return "", "", fmt.Errorf("revoke refresh token: %w", err)
	}

	// Issue a fresh token in the same family.
	newJTI, err := randomID()
	if err != nil {
		return "", "", err
	}
	newToken, err := s.jwt.IssueRefreshToken(userID, newJTI, familyID)
	if err != nil {
		return "", "", err
	}
	if _, err := s.pg.Exec(ctx,
		`INSERT INTO refresh_tokens (jti, user_id, family_id, expires_at)
		 VALUES ($1, $2, $3, now() + make_interval(secs => $4))`,
		newJTI, userID, familyID, int(s.ttl.Seconds())); err != nil {
		return "", "", fmt.Errorf("store rotated refresh token: %w", err)
	}
	return userID, newToken, nil
}

// revokeFamily revokes every token in a family, used when reuse is detected.
func (s *RefreshTokenStore) revokeFamily(ctx context.Context, familyID string) {
	if _, err := s.pg.Exec(ctx,
		`UPDATE refresh_tokens SET revoked = true WHERE family_id = $1`, familyID); err != nil {
		// best-effort; logged elsewhere if needed
		_ = err
	}
}

// randomID returns a 32-char hex string suitable for jti / family_id.
func randomID() (string, error) {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generate id: %w", err)
	}
	return hex.EncodeToString(b), nil
}
