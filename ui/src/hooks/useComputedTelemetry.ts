import { useEffect, useState, useCallback } from 'react'
import { supabase } from '../lib/supabase'
import type { ComputedValues } from '../lib/computedTelemetry'

interface ComputedRow {
  id: number
  device_key: string
  recorded_at: string
  pv_power: number; battery_power: number
  battery_charging_power: number; battery_discharging_power: number
  dc_load_power: number; unclassified_power: number
  inverter_power: number
  system_status: 'unknown' | 'charging' | 'discharging' | 'balanced'
  min_soc_pct: number | null; max_soc_pct: number | null
  total_energy_wh: number
  // typed columns
  ch0_v: number | null; ch0_i: number | null; ch0_p: number | null
  ch1_v: number | null; ch1_i: number | null; ch1_p: number | null
  ch2_v: number | null; ch2_i: number | null; ch2_p: number | null
  ch3_v: number | null; ch3_i: number | null; ch3_p: number | null
  energy_wh0: number | null; energy_wh1: number | null; energy_wh2: number | null; energy_wh3: number | null
  soc_pct0: number | null; soc_pct1: number | null; soc_pct2: number | null; soc_pct3: number | null
}

const HISTORY_LIMIT = 200

export function useComputedTelemetry(deviceKey: string | null) {
  const [computedLatest, setComputedLatest] = useState<ComputedValues | null>(null)
  const [computedHistory, setComputedHistory] = useState<ComputedValues[]>([])
  const [isLoading, setIsLoading] = useState(false)

  const recompute = useCallback((row: ComputedRow): ComputedValues => {
    return {
      pv_power: row.pv_power ?? 0,
      battery_power: row.battery_power ?? 0,
      battery_charging_power: row.battery_charging_power ?? 0,
      battery_discharging_power: row.battery_discharging_power ?? 0,
      dc_load_power: row.dc_load_power ?? 0,
      unclassified_power: row.unclassified_power ?? 0,
      inverter_power: row.inverter_power ?? 0,
      system_status: row.system_status ?? 'unknown',
      min_soc_pct: row.min_soc_pct,
      max_soc_pct: row.max_soc_pct,
      total_energy_wh: row.total_energy_wh ?? 0,
    }
  }, [])

  useEffect(() => {
    if (!deviceKey) {
      setComputedLatest(null)
      setComputedHistory([])
      return
    }
    setIsLoading(true)

    // Load recent computed rows (server-side precomputed)
    supabase
      .from('telemetry_computed')
      .select('*')
      .eq('device_key', deviceKey)
      .order('recorded_at', { ascending: false })
      .limit(HISTORY_LIMIT)
      .then(({ data }) => {
        if (data) {
          const rows = (data as ComputedRow[]).slice().reverse()
          const history = rows.map(r => recompute(r))
          setComputedHistory(history)
          if (history.length > 0) {
            setComputedLatest(history[history.length - 1])
          }
        }
        setIsLoading(false)
      })

    // Subscribe to telemetry_computed (typed columns, server-computed aggregates)
    const channel = supabase
      .channel(`telemetry-computed-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_computed',
        filter: `device_key=eq.${deviceKey}`,
      }, payload => {
        const row = payload.new as ComputedRow
        const computed = recompute(row)
        setComputedLatest(computed)
        setComputedHistory(prev => {
          const next = [...prev, computed]
          return next.length > HISTORY_LIMIT ? next.slice(-HISTORY_LIMIT) : next
        })
      })
      .subscribe()

    return () => {
      supabase.removeChannel(channel)
    }
  }, [deviceKey, recompute])

  if (!deviceKey) {
    return { computedLatest: null, computedHistory: [], isLoading: false }
  }

  return { computedLatest, computedHistory, isLoading }
}