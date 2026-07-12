// internal/alerts_test.go — Tests for alert engine.

package internal

import "testing"

func TestEvaluateCondition(t *testing.T) {
	e := &AlertEngine{}
	tests := []struct {
		value     float64
		op        string
		threshold float64
		want      bool
	}{
		{10, "gt", 5, true},
		{3, "gt", 5, false},
		{5, "gte", 5, true},
		{5, "lt", 10, true},
		{5, "eq", 5, true},
		{5, "neq", 10, true},
	}
	for _, tt := range tests {
		got := e.evaluateCondition(tt.value, tt.op, tt.threshold)
		if got != tt.want {
			t.Errorf("evaluateCondition(%v, %q, %v) = %v, want %v", tt.value, tt.op, tt.threshold, got, tt.want)
		}
	}
}

func TestRequiredSamples(t *testing.T) {
	if got := requiredSamples(0); got != 1 {
		t.Errorf("requiredSamples(0) = %d, want 1", got)
	}
	if got := requiredSamples(5); got != 1 {
		t.Errorf("requiredSamples(5) = %d, want 1", got)
	}
	if got := requiredSamples(6); got != 2 {
		t.Errorf("requiredSamples(6) = %d, want 2", got)
	}
}
