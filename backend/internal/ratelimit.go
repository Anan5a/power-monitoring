// internal/ratelimit.go — Token bucket rate limiter.
// Per-IP for auth endpoints, per-token for API endpoints.

package internal

import (
	"net/http"
	"strconv"
	"sync"
	"time"
)

type RateLimiter struct {
	mu      sync.Mutex
	buckets map[string]*tokenBucket
	maxRate int
	refill  time.Duration
}

type tokenBucket struct {
	tokens    float64
	lastRefill time.Time
}

func NewRateLimiter(maxRate int, refill time.Duration) *RateLimiter {
	return &RateLimiter{
		buckets: make(map[string]*tokenBucket),
		maxRate: maxRate,
		refill:  refill,
	}
}

func (rl *RateLimiter) Allow(key string) bool {
	rl.mu.Lock()
	defer rl.mu.Unlock()

	b, ok := rl.buckets[key]
	if !ok {
		b = &tokenBucket{tokens: float64(rl.maxRate), lastRefill: time.Now()}
		rl.buckets[key] = b
	}

	// Refill
	elapsed := time.Since(b.lastRefill).Seconds()
	b.tokens += elapsed * (float64(rl.maxRate) / rl.refill.Seconds())
	if b.tokens > float64(rl.maxRate) {
		b.tokens = float64(rl.maxRate)
	}
	b.lastRefill = time.Now()

	if b.tokens >= 1 {
		b.tokens--
		return true
	}
	return false
}

// RateLimitMiddleware limits requests per IP. Use for auth endpoints.
func RateLimitMiddleware(maxRate int, refill time.Duration) func(http.Handler) http.Handler {
	limiter := NewRateLimiter(maxRate, refill)
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			key := r.RemoteAddr
			if !limiter.Allow(key) {
				w.Header().Set("Retry-After", strconv.Itoa(int(refill.Seconds())))
				writeError(w, "rate_limited", "too many requests", http.StatusTooManyRequests)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}
