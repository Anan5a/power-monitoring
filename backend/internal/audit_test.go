// internal/audit_test.go — Tests for audit log utilities.

package internal

import "testing"

func TestNullIfEmpty(t *testing.T) {
	if v := nullIfEmpty(""); v != nil {
		t.Error("nullIfEmpty('') should return nil")
	}
	if v := nullIfEmpty("hello"); v == nil || *v != "hello" {
		t.Error("nullIfEmpty('hello') should return pointer to 'hello'")
	}
}
