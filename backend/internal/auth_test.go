// internal/auth_test.go — Tests for password hashing and JWT management.

package internal

import (
	"testing"
	"time"
)

func TestHashPassword(t *testing.T) {
	hash, err := HashPassword("my-secret-password")
	if err != nil {
		t.Fatalf("HashPassword() error = %v", err)
	}
	if hash == "" {
		t.Fatal("HashPassword() returned empty hash")
	}
	if !CheckPassword(hash, "my-secret-password") {
		t.Error("CheckPassword() = false, want true")
	}
	if CheckPassword(hash, "wrong-password") {
		t.Error("CheckPassword() = true, want false")
	}
}

func TestJWTManager_IssueAndValidate(t *testing.T) {
	mgr := NewJWTManager("test-secret-that-is-at-least-32-characters!!", time.Hour, 720*time.Hour)

	access, err := mgr.IssueAccessToken("user-1", "user")
	if err != nil {
		t.Fatalf("IssueAccessToken() error = %v", err)
	}
	if access == "" {
		t.Fatal("IssueAccessToken() returned empty token")
	}

	claims, err := mgr.ValidateToken(access)
	if err != nil {
		t.Fatalf("ValidateToken() error = %v", err)
	}
	if claims.UserID != "user-1" {
		t.Errorf("UserID = %q, want user-1", claims.UserID)
	}
	if claims.Role != "user" {
		t.Errorf("Role = %q, want user", claims.Role)
	}
}

func TestJWTManager_ExpiredToken(t *testing.T) {
	mgr := NewJWTManager("test-secret-that-is-at-least-32-characters!!", -time.Hour, 720*time.Hour)
	token, _ := mgr.IssueAccessToken("user-1", "user")
	_, err := mgr.ValidateToken(token)
	if err == nil {
		t.Error("ValidateToken() expected error for expired token")
	}
}
