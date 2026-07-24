// internal/oauth_test.go — Tests for OAuth handler.

package internal

import (
	"net/http"
	"net/http/httptest"
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

func TestOAuthCallback_UnsupportedProvider(t *testing.T) {
	h := NewOAuthHandler(nil, nil, nil, "", "", "", "", "http://localhost:8080")
	req := httptest.NewRequest("GET", "/api/v1/auth/oauth/unknown/callback?code=x&state=y", nil)
	rec := httptest.NewRecorder()
	h.Callback(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("status = %d, want 404 for unsupported provider", rec.Code)
	}
}

func TestOAuthRedirect_UnsupportedProvider(t *testing.T) {
	h := NewOAuthHandler(nil, nil, nil, "", "", "", "", "http://localhost:8080")
	req := httptest.NewRequest("GET", "/api/v1/auth/oauth/unknown", nil)
	rec := httptest.NewRecorder()
	h.Redirect(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("status = %d, want 404 for unsupported provider", rec.Code)
	}
}
