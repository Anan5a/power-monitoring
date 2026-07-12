// internal/retention_test.go — Tests for retention cleanup.

package internal

import "testing"

func TestRetentionCleanup_NoDevices(t *testing.T) {
	// With no devices, Run should not panic
	rc := &RetentionCleanup{}
	err := rc.Run(nil)
	if err != nil {
		t.Errorf("Run() error = %v, want nil", err)
	}
}
