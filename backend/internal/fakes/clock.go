// internal/fakes/clock.go — Deterministic clock for tests.
// Replace time.Now() in production code with this interface so
// time-based logic (quiet hours, retention, JWT expiry) is testable.

package fakes

import "time"

// FixedClock returns a fixed time. Use in tests where time must be deterministic.
type FixedClock struct {
	T time.Time
}

func (c FixedClock) Now() time.Time { return c.T }

// ParseClock creates a FixedClock from an ISO 8601 string. Panics on bad input.
func ParseClock(iso string) FixedClock {
	t, err := time.Parse(time.RFC3339, iso)
	if err != nil {
		panic(err)
	}
	return FixedClock{T: t}
}
