// internal/email_test.go — Tests for email template rendering.

package internal

import "testing"

func TestRenderText(t *testing.T) {
	result := renderText("Hello {{.Name}}", map[string]any{"Name": "World"})
	if result != "Hello World" {
		t.Errorf("renderText = %q, want 'Hello World'", result)
	}
}

func TestRenderText_MissingVar(t *testing.T) {
	result := renderText("Hello {{.Name}}", nil)
	if result != "Hello <no value>" {
		t.Errorf("renderText = %q, want 'Hello <no value>'", result)
	}
}
