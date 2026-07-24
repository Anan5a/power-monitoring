// internal/websocket_test.go — Tests for WebSocket hub.

package internal

import "testing"

func TestWebSocketHub_Broadcast(t *testing.T) {
	hub := NewWebSocketHub(nil)
	// No connected clients — broadcast should not panic
	hub.Broadcast("AABBCCDDEEFF", []byte(`{"test": true}`))
	// If we got here without panic, the test passes
}
