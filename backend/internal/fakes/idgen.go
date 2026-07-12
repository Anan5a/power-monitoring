// internal/fakes/idgen.go — Predictable ID generator for tests.

package fakes

import "fmt"

// SequentialIDGen returns IDs like "00000000-0000-0000-0000-000000000001".
type SequentialIDGen struct {
	counter int
}

func (g *SequentialIDGen) New() string {
	g.counter++
	return fmt.Sprintf("00000000-0000-0000-0000-000000%06d", g.counter)
}
