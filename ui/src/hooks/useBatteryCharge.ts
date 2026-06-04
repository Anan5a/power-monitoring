import { useEffect, useState, useRef } from 'react'
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
  const pollIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)

  async function fetchCharge() {
    if (!deviceId) return
    const { data, error } = await supabase.rpc('get_battery_charge', { p_device_id: deviceId, p_hours: 24 })
    if (error || !data) return
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
  }

  useEffect(() => {
    if (!deviceId) {
      setState(s => ({ ...s, isLoading: false }))
      return
    }
    fetchCharge()
    // Poll every 10 seconds so SoC updates in real-time
    pollIntervalRef.current = setInterval(fetchCharge, 10000)
    return () => {
      if (pollIntervalRef.current) clearInterval(pollIntervalRef.current)
    }
  }, [deviceId])

  return state
}