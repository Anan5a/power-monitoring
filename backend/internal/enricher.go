// internal/enricher.go — Classifies raw channel readings into PV/battery/load
// groups based on the device's channel_groups configuration. Pure logic — no I/O.

package internal

import "math"

// Enricher is a stateless pure-function helper that turns the firmware's
// flat per-channel readings into the derived aggregate fields the API,
// store, and UI consume. It holds no configuration of its own; the
// channel_groups slice is supplied per call so device config can change
// without restarting the ingest worker.
type Enricher struct{}

// NewEnricher returns a ready-to-use Enricher. The value is stateless so
// all Enrichers are equivalent; the constructor exists for symmetry with
// the other pipeline components.
func NewEnricher() *Enricher { return &Enricher{} }

// Enrich classifies each channel's power into PV, battery, or load based on
// the device's channel_groups. Channels not in any group fall to battery
// fallback (if a battery profile exists) or unclassified.
//
// Sign convention used throughout:
//   - Positive power on a PV channel means generation (added to pvPower).
//   - Positive power on a battery channel means charging; negative means
//     discharging. battery_power is reported as charge - discharge, so a
//     positive result means net charging.
//   - Positive power on a load channel means consumption; the raw value is
//     negated because the firmware reports load as a negative draw.
func (e *Enricher) Enrich(fields map[string]float64, groups []ChannelGroup) EnrichmentResult {
	var pvPower, batteryCharge, batteryDischarge, dcLoad float64

	// The firmware exposes exactly four channels (ch0..ch3); the loop is
	// fixed-range because the field keys are ch{0..3}_P.
	for ch := 0; ch < 4; ch++ {
		power := getField(fields, chPowerKey(ch))
		if power == 0 {
			continue
		}
		classified := false
		// A channel may belong to at most one group; the first matching
		// group wins and we break out of the inner loop.
		for _, g := range groups {
			// Channel membership is a bitmask: bit ch is set if channel ch
			// belongs to this group.
			if g.ChannelMask&(1<<ch) != 0 {
				switch g.Icon {
				case 0: // solar
					pvPower += max(0, power)
				case 1: // battery
					// Split signed power into charge/discharge buckets so we
					// can report both directions without losing magnitude.
					if power > 0 {
						batteryCharge += power
					} else {
						batteryDischarge += -power
					}
				case 2: // load
					// Firmware reports load as a negative draw; flip sign to
					// get a positive consumption figure.
					dcLoad += max(0, -power)
				}
				classified = true
				break
			}
		}
		if !classified {
			// Fallback: treat as battery. Any channel the user has not
			// assigned to a group is assumed to be a battery channel, which
			// matches the default firmware wiring for a single-battery bank.
			if power > 0 {
				batteryCharge += power
			} else {
				batteryDischarge += -power
			}
		}
	}

	// Inverter power is the net flow at the AC bus: what PV generates plus
	// what the battery discharges, minus what goes into charging the battery
	// and what the DC loads consume directly. A positive value means net
	// export to the AC side; negative means net import from the grid.
	inverterPower := pvPower + batteryDischarge - batteryCharge - dcLoad

	// System status is a coarse 4-state enum derived from the power balance.
	// The 5W thresholds avoid flicker around zero from sensor noise.
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

// chPowerKey returns the firmware's JSON key for channel ch's power reading.
// Panics for ch outside [0,3] — the loop above never produces such a value.
func chPowerKey(ch int) string {
	return []string{"ch0_P", "ch1_P", "ch2_P", "ch3_P"}[ch]
}

// getField returns fields[key], or 0 if the key is absent. The firmware
// omits channels that are disabled or missing their sensor, so a missing
// key is treated as "no reading" rather than an error.
func getField(fields map[string]float64, key string) float64 {
	if v, ok := fields[key]; ok {
		return v
	}
	return 0
}

// minSOC returns the lowest state-of-charge across all four channel slots.
// Defaults to 100 when no SoC readings are present so an unconfigured device
// does not falsely trigger low-battery alerts.
func minSOC(fields map[string]float64) float32 {
	min := float64(100)
	for _, k := range []string{"soc_pct0", "soc_pct1", "soc_pct2", "soc_pct3"} {
		if v, ok := fields[k]; ok && v < min {
			min = v
		}
	}
	return float32(min)
}

// maxSOC returns the highest state-of-charge across all four channel slots.
// Defaults to 0 when no SoC readings are present, mirroring minSOC's logic.
func maxSOC(fields map[string]float64) float32 {
	max := float64(0)
	for _, k := range []string{"soc_pct0", "soc_pct1", "soc_pct2", "soc_pct3"} {
		if v, ok := fields[k]; ok && v > max {
			max = v
		}
	}
	return float32(max)
}

// totalEnergy sums the energy counters (energy_whN) across all four channels
// to give a single system-wide energy figure for billing/ dashboards.
func totalEnergy(fields map[string]float64) float32 {
	var total float64
	for _, k := range []string{"energy_wh0", "energy_wh1", "energy_wh2", "energy_wh3"} {
		total += getField(fields, k)
	}
	return float32(total)
}
