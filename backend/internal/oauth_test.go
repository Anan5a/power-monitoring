// internal/oauth_test.go — Tests for OAuth handler.

package internal

import "testing"

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
