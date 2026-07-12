package fakes

import "testing"

func TestDeviceBuilder(t *testing.T) {
	dev := aDevice("AABBCCDDEEFF").ownedBy("user-1").build()
	if dev.DeviceKey != "AABBCCDDEEFF" {
		t.Errorf("DeviceKey = %q, want AABBCCDDEEFF", dev.DeviceKey)
	}
	if dev.OwnerID == nil || *dev.OwnerID != "user-1" {
		t.Errorf("OwnerID = %v, want user-1", dev.OwnerID)
	}
}
