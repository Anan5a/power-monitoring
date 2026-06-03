import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'

// Battery charge state derived from integrating battery_power over time.
// Firmware coulomb counter is broken/unreliable, so we compute charge from telemetry.
//
// Approach:
//   - Use current SOC% as the anchor (assumed trustworthy at any given moment)
//   - Integrate battery_power forward/backward from anchor using historical power readings
//   - Positive battery_power = charging (adding Wh), negative = discharging (removing Wh)
//
// Limitation: power sensor errors accumulate over time. The SOC anchor recalibrates
// the estimate each time a trustworthy SOC reading arrives from the BMS.

export interface BatteryChargeState {
  chargeWh: number        // current estimated charge in Wh
  capacityWh: number      // nominal capacity in Wh
  socAnchor: number       // SOC % used as anchor (most recent reading)
  isLoading: boolean
}

export function useBatteryCharge(
  deviceKey: string | null,
  socPct: number | null,   // soc_pct0 from realtime data
  batteryCapacityWh: number // from battery profile
) {
  const [state, setState] = useState<BatteryChargeState>({
    chargeWh: 0,
    capacityWh: 0,
    socAnchor: 0,
    isLoading: true,
  })
  const anchorSocRef = useRef<number | null>(null)

  useEffect(() => {
    if (!deviceKey || socPct === null || batteryCapacityWh <= 0) {
      setState(s => ({ ...s, isLoading: false, capacityWh: batteryCapacityWh }))
      return
    }

    // Only refetch if anchor SOC changed significantly (>2% drift)
    if (anchorSocRef.current !== null && Math.abs(anchorSocRef.current - socPct) < 2) {
      return
    }
    anchorSocRef.current = socPct

    const now = new Date()
    const since = new Date(now.getTime() - 6 * 3600 * 1000) // last 6h of history

    supabase
      .from('telemetry_computed')
      .select('recorded_at, battery_power, soc_pct0')
      .eq('device_key', deviceKey)
      .gte('recorded_at', since.toISOString())
      .order('recorded_at', { ascending: true })
      .then(({ data, error }) => {
        if (error || !data || data.length === 0) {
          // No history, fall back to SOC anchor only
          setState({
            chargeWh: (socPct / 100) * batteryCapacityWh,
            capacityWh: batteryCapacityWh,
            socAnchor: socPct,
            isLoading: false,
          })
          return
        }

        // Anchor: current SOC gives us the best estimate right now
        const anchorChargeWh = (socPct / 100) * batteryCapacityWh

        // Integrate power readings backward from now to estimate charge at anchor time
        // battery_power > 0 = charging, < 0 = discharging
        let integratedChargeDeltaWh = 0
        let prevTs: Date | null = null

        // Walk from oldest to newest, accumulating energy in/out
        for (const row of data as { recorded_at: string; battery_power: number | null }[]) {
          if (row.battery_power == null) continue
          const ts = new Date(row.recorded_at)
          if (prevTs) {
            const dtHours = (ts.getTime() - prevTs.getTime()) / 3600000
            if (dtHours > 0 && dtHours < 1) { // sanity check: <1h gaps only
              integratedChargeDeltaWh += row.battery_power * dtHours
            }
          }
          prevTs = ts
        }

        // The anchor SOC was measured at the most recent row.
        // We integrate backward from now (where we trust SOC) to get the charge
        // at the start of our window. Then the accumulated delta tells us how
        // much energy has gone in/out since.
        //
        // netChargeWh = anchorChargeWh - integratedChargeDeltaWh
        // (subtract because integratedChargeDeltaWh is what the battery HAS done since anchor)
        const netChargeWh = anchorChargeWh - integratedChargeDeltaWh

        setState({
          chargeWh: Math.max(0, Math.min(batteryCapacityWh, netChargeWh)),
          capacityWh: batteryCapacityWh,
          socAnchor: socPct,
          isLoading: false,
        })
      })
  }, [deviceKey, socPct, batteryCapacityWh])

  return state
}