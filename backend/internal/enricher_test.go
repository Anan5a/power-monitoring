// internal/enricher_test.go — Tests for channel classification.

package internal

import "testing"

func TestEnricher_ClassifiesSolarChannel(t *testing.T) {
	e := &Enricher{}
	groups := []ChannelGroup{
		{Icon: 0, ChannelMask: 0b0001}, // ch0 = solar
		{Icon: 1, ChannelMask: 0b0010}, // ch1 = battery
		{Icon: 2, ChannelMask: 0b0100}, // ch2 = load
	}
	fields := map[string]float64{
		"ch0_P": 19.8,
		"ch1_P": -6.4,
		"ch2_P": -12.0,
	}

	result := e.Enrich(fields, groups)

	if result.PVPower != 19.8 {
		t.Errorf("PVPower = %f, want 19.8", result.PVPower)
	}
	if result.BatteryPower != -6.4 {
		t.Errorf("BatteryPower = %f, want -6.4", result.BatteryPower)
	}
	if result.DCLoadPower != 12.0 {
		t.Errorf("DCLoadPower = %f, want 12.0", result.DCLoadPower)
	}
}
