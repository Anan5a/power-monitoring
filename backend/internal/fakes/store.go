// internal/fakes/store.go — In-memory telemetry store for pipeline tests.

package fakes

import (
	"context"
	"sync"

	"github.com/yourorg/iot-platform/internal"
)

type MemStore struct {
	mu   sync.Mutex
	Rows []internal.TelemetryRow
}

func (s *MemStore) Write(_ context.Context, row internal.TelemetryRow) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.Rows = append(s.Rows, row)
	return nil
}

func (s *MemStore) Flush(_ context.Context) error { return nil }

func NewMemStore() *MemStore { return &MemStore{} }

func (s *MemStore) Count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.Rows)
}
