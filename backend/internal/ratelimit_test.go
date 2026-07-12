// internal/ratelimit_test.go — Tests for token bucket rate limiter.

package internal

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestRateLimiter_AllowsWithinLimit(t *testing.T) {
	rl := NewRateLimiter(5, time.Minute)
	for i := 0; i < 5; i++ {
		if !rl.Allow("test-key") {
			t.Errorf("request %d denied, want allowed", i+1)
		}
	}
}

func TestRateLimiter_BlocksAfterLimit(t *testing.T) {
	rl := NewRateLimiter(2, time.Minute)
	rl.Allow("test-key")
	rl.Allow("test-key")
	if rl.Allow("test-key") {
		t.Error("third request allowed, want denied")
	}
}

func TestRateLimitMiddleware(t *testing.T) {
	handler := RateLimitMiddleware(1, time.Minute)(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest("GET", "/api/v1/auth/login", nil)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("first request: status = %d, want 200", rec.Code)
	}

	rec2 := httptest.NewRecorder()
	handler.ServeHTTP(rec2, req)
	if rec2.Code != http.StatusTooManyRequests {
		t.Errorf("second request: status = %d, want 429", rec2.Code)
	}
}
