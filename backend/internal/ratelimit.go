// internal/ratelimit.go — Token bucket rate limiter.
// Per-IP for auth endpoints, per-token for API endpoints.

package internal

import (
	"net"
	"net/http"
	"strconv"
	"sync"
	"time"
)

// RateLimiter is a token-bucket limiter shared across many keys (per-IP for
// auth endpoints, per-token for API endpoints). Buckets are created lazily and
// never garbage-collected, so callers should use one limiter per logical scope
// rather than a single global one for unrelated keys.
type RateLimiter struct {
	mu      sync.Mutex
	buckets map[string]*tokenBucket
	maxRate int
	refill  time.Duration
}

// tokenBucket holds the current token balance and the timestamp of the last
// refill. Tokens are refilled lazily on each Allow call rather than by a
// background goroutine so the limiter is safe to create and forget.
type tokenBucket struct {
	tokens     float64
	lastRefill time.Time
}

// NewRateLimiter returns a limiter that grants up to maxRate tokens and
// replenishes one token every refill/maxRate of time (i.e. a full refill
// takes `refill` once the bucket is empty).
func NewRateLimiter(maxRate int, refill time.Duration) *RateLimiter {
	return &RateLimiter{
		buckets: make(map[string]*tokenBucket),
		maxRate: maxRate,
		refill:  refill,
	}
}

// Allow reports whether a single request for the given key is permitted,
// consuming one token if so. It is safe for concurrent use; the mutex makes
// the read-modify-write on each bucket atomic.
func (rl *RateLimiter) Allow(key string) bool {
	rl.mu.Lock()
	defer rl.mu.Unlock()

	b, ok := rl.buckets[key]
	if !ok {
		// New buckets start full so a fresh client is not throttled on its
		// first burst of requests.
		b = &tokenBucket{tokens: float64(rl.maxRate), lastRefill: time.Now()}
		rl.buckets[key] = b
	}

	// Refill proportionally to elapsed wall-clock time, then cap at maxRate so
	// long idle periods do not let a client accumulate an unbounded burst.
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
			// Rate-limit by IP only (strip the ephemeral port from RemoteAddr so
			// each request from the same client does not get its own bucket).
			key, _, err := net.SplitHostPort(r.RemoteAddr)
			if err != nil {
				key = r.RemoteAddr // already host-only (e.g. behind RealIP)
			}
			if !limiter.Allow(key) {
				w.Header().Set("Retry-After", strconv.Itoa(int(refill.Seconds())))
				writeError(w, "rate_limited", "too many requests", http.StatusTooManyRequests)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}
