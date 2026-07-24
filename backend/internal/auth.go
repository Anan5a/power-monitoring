// internal/auth.go — JWT token management and password hashing.
// Uses golang-jwt for tokens and bcrypt for passwords.

package internal

import (
	"fmt"
	"sync"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"golang.org/x/crypto/bcrypt"
)

// ── Password hashing ────────────────────────────────────────────────

// bcryptCost is the bcrypt cost factor used for hashing. 12 is intentionally
// high enough to make offline cracking expensive while keeping login latency
// acceptable on the API server; raise it if hardware allows.
const bcryptCost = 12

// HashPassword returns a bcrypt hash of password using the configured cost.
func HashPassword(password string) (string, error) {
	bytes, err := bcrypt.GenerateFromPassword([]byte(password), bcryptCost)
	if err != nil {
		return "", fmt.Errorf("bcrypt hash: %w", err)
	}
	return string(bytes), nil
}

// CheckPassword reports whether password matches the given bcrypt hash.
// It relies on bcrypt.CompareHashAndPassword, which runs in constant time
// relative to the hash and returns nil only on an exact match.
func CheckPassword(hash, password string) bool {
	return bcrypt.CompareHashAndPassword([]byte(hash), []byte(password)) == nil
}

// dummyBcryptHash is a valid bcrypt digest (generated once, lazily) used to
// keep the login missing-user path constant-time. Comparing a supplied
// password against it always fails but still runs the bcrypt KDF, so the
// response time for a nonexistent email matches that of a wrong password on
// an existing email — preventing user enumeration via timing.
var (
	dummyHashOnce sync.Once
	dummyHash     string
)

// DummyBcryptHash returns a lazily-generated valid bcrypt digest used to keep
// the login "user not found" path constant-time with the "wrong password" path.
func DummyBcryptHash() string {
	dummyHashOnce.Do(func() {
		h, err := HashPassword("constant-time-dummy-do-not-match")
		if err != nil {
			// Fallback: an empty hash makes CheckPassword return false quickly,
			// but generation only fails on extreme misconfiguration.
			dummyHash = ""
			return
		}
		dummyHash = h
	})
	return dummyHash
}

// ── JWT ─────────────────────────────────────────────────────────────

// JWTManager issues and validates HMAC-SHA256 signed JSON Web Tokens. A single
// signing secret is shared by access and refresh tokens; the two are
// distinguished by their claims (refresh tokens carry a jti/family_id) and by
// the server-side refresh_tokens table, not by the signing algorithm.
type JWTManager struct {
	secret     []byte
	accessTTL  time.Duration
	refreshTTL time.Duration
}

// NewJWTManager builds a JWTManager with the given HMAC secret and token TTLs.
func NewJWTManager(secret string, accessTTL, refreshTTL time.Duration) *JWTManager {
	return &JWTManager{
		secret:     []byte(secret),
		accessTTL:  accessTTL,
		refreshTTL: refreshTTL,
	}
}

// Claims is the JWT claims payload extended with application-specific fields.
type Claims struct {
	// UserID is the subject's local user identifier; stored in the JWT for
	// fast request authentication without a DB lookup.
	UserID string `json:"user_id"`
	// Role is the authorization role ("admin" or "user") used by AdminOnly.
	Role string `json:"role"`
	// FamilyID groups refresh tokens issued by the same login chain so the
	// refresh-token store can revoke them together on reuse detection.
	FamilyID string `json:"family_id,omitempty"` // refresh-token family (rotation)
	jwt.RegisteredClaims
}

// IssueAccessToken signs a short-lived access token for userID with the
// given role. Access tokens are stateless; their lifetime is accessTTL.
func (m *JWTManager) IssueAccessToken(userID, role string) (string, error) {
	now := time.Now()
	claims := Claims{
		UserID: userID,
		Role:   role,
		RegisteredClaims: jwt.RegisteredClaims{
			ExpiresAt: jwt.NewNumericDate(now.Add(m.accessTTL)),
			IssuedAt:  jwt.NewNumericDate(now),
			Issuer:    "iot-platform",
		},
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString(m.secret)
}

// IssueRefreshToken signs a refresh token carrying a jti (token id) and
// family_id so the server-side store can rotate and revoke it.
func (m *JWTManager) IssueRefreshToken(userID, jti, familyID string) (string, error) {
	now := time.Now()
	claims := Claims{
		UserID:   userID,
		FamilyID: familyID,
		RegisteredClaims: jwt.RegisteredClaims{
			ID:        jti,
			ExpiresAt: jwt.NewNumericDate(now.Add(m.refreshTTL)),
			IssuedAt:  jwt.NewNumericDate(now),
			Issuer:    "iot-platform",
		},
	}
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	return token.SignedString(m.secret)
}

// ValidateToken parses and verifies the signature/expiry of tokenStr and
// returns its claims. It rejects any token whose declared signing algorithm
// is not HMAC-SHA (e.g. "none" or an RSA public-key attack) before returning
// the key, so attackers cannot bypass verification by claiming a different alg.
func (m *JWTManager) ValidateToken(tokenStr string) (*Claims, error) {
	token, err := jwt.ParseWithClaims(tokenStr, &Claims{}, func(t *jwt.Token) (any, error) {
		// Pin the expected signing method: refuse to verify with the HMAC key
		// if the token header advertises anything other than an HMAC method.
		if _, ok := t.Method.(*jwt.SigningMethodHMAC); !ok {
			return nil, fmt.Errorf("unexpected signing method: %v", t.Header["alg"])
		}
		return m.secret, nil
	})
	if err != nil {
		return nil, fmt.Errorf("parse token: %w", err)
	}
	claims, ok := token.Claims.(*Claims)
	if !ok || !token.Valid {
		return nil, fmt.Errorf("invalid token")
	}
	return claims, nil
}
