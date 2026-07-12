// internal/enricher.go — Classifies raw channel readings into PV/battery/load
// groups based on the device's channel_groups configuration. Pure logic — no I/O.

package internal

import "math"

type Enricher struct{}

func NewEnricher() *Enricher { return &Enricher{} }

// Enrich classifies each channel's power into PV, battery, or load based on
// the device's channel_groups. Channels not in any group fall to battery
// fallback (if a battery profile exists) or unclassified.
func (e *Enricher) Enrich(fields map[string]float64, groups []ChannelGroup) EnrichmentResult {
	var pvPower, batteryCharge, batteryDischarge, dcLoad float64

	for ch := 0; ch < 4; ch++ {
		power := getField(fields, chPowerKey(ch))
		if power == 0 {
			continue
		}
		classified := false
		for _, g := range groups {
			if g.ChannelMask&(1<<ch) != 0 {
				switch g.Icon {
				case 0: // solar
					pvPower += max(0, power)
				case 1: // battery
					if power > 0 {
						batteryCharge += power
					} else {
						batteryDischarge += -power
					}
				case 2: // load
					dcLoad += max(0, -power)
				}
				classified = true
				break
			}
		}
		if !classified {
			// Fallback: treat as battery
			if power > 0 {
				batteryCharge += power
			} else {
				batteryDischarge += -power
			}
		}
	}

	inverterPower := pvPower + batteryDischarge - batteryCharge - dcLoad

	var status uint8
	if batteryCharge > 5 {
		status = 1 // charging
	} else if batteryDischarge > 5 {
		status = 2 // discharging
	} else if math.Abs(inverterPower) <= 5 {
		status = 3 // balanced
	}

	return EnrichmentResult{
		PVPower:       float32(pvPower),
		BatteryPower:  float32(batteryCharge - batteryDischarge),
		InverterPower: float32(inverterPower),
		DCLoadPower:   float32(dcLoad),
		SystemStatus:  status,
		MinSOCPct:     minSOC(fields),
		MaxSOCPct:     maxSOC(fields),
		TotalEnergyWh: totalEnergy(fields),
	}
}

func chPowerKey(ch int) string {
	return []string{"ch0_P", "ch1_P", "ch2_P", "ch3_P"}[ch]
}

func getField(fields map[string]float64, key string) float64 {
	if v, ok := fields[key]; ok {
		return v
	}
	return 0
}

func minSOC(fields map[string]float64) float32 {
	min := float64(100)
	for _, k := range []string{"soc_pct0", "soc_pct1", "soc_pct2", "soc_pct3"} {
		if v, ok := fields[k]; ok && v < min {
			min = v
		}
	}
	return float32(min)
}

func maxSOC(fields map[string]float64) float32 {
	max := float64(0)
	for _, k := range []string{"soc_pct0", "soc_pct1", "soc_pct2", "soc_pct3"} {
		if v, ok := fields[k]; ok && v > max {
			max = v
		}
	}
	return float32(max)
}

func totalEnergy(fields map[string]float64) float32 {
	var total float64
	for _, k := range []string{"energy_wh0", "energy_wh1", "energy_wh2", "energy_wh3"} {
		total += getField(fields, k)
	}
	return float32(total)
}
