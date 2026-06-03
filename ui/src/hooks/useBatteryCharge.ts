import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'

export interface BatteryChargeState {
  chargeWh: number
  capacityWh: number
  energyIn24h: number
  energyOut24h: number
  socPct: number
  isFullChargeToday: boolean
  isLoading: boolean
}

export function useBatteryCharge(deviceId: string | null) {
  const [state, setState] = useState<BatteryChargeState>({
    chargeWh: 0,
    capacityWh: 0,
    energyIn24h: 0,
    energyOut24h: 0,
    socPct: 0,
    isFullChargeToday: false,
    isLoading: true,
  })

  useEffect(() => {
    if (!deviceId) {
      setState(s => ({ ...s, isLoading: false }))
      return
    }

    supabase.rpc('get_battery_charge', { p_device_id: deviceId, p_hours: 24 })
      .then(({ data, error }) => {
        if (error || !data) {
          setState(s => ({ ...s, isLoading: false }))
          return
        }
        const row = Array.isArray(data) ? data[0] : data
        setState({
          chargeWh: row.charge_wh ?? 0,
          capacityWh: row.capacity_wh ?? 0,
          energyIn24h: row.energy_in_24h ?? 0,
          energyOut24h: row.energy_out_24h ?? 0,
          socPct: row.soc_pct ?? 0,
          isFullChargeToday: row.is_full_charge_today ?? false,
          isLoading: false,
        })
      })
  }, [deviceId])

  return state
}