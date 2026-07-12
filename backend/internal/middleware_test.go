// internal/middleware_test.go — Tests for HTTP middleware.

package internal

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestAuthMiddleware_NoToken(t *testing.T) {
	jwt := NewJWTManager("test-secret-that-is-at-least-32-characters!!", 0, 0)
	handler := AuthMiddleware(jwt)(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest("GET", "/api/v1/devices", nil)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Errorf("status = %d, want 401", rec.Code)
	}
}

func TestAuthMiddleware_ValidToken(t *testing.T) {
	jwt := NewJWTManager("test-secret-that-is-at-least-32-characters!!", time.Hour, 0)
	token, _ := jwt.IssueAccessToken("user-1", "user")

	handler := AuthMiddleware(jwt)(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Context().Value(ContextUserID) != "user-1" {
			t.Error("ContextUserID not set")
		}
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest("GET", "/api/v1/devices", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status = %d, want 200", rec.Code)
	}
}
