import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

const HISTORY_LIMIT = 200

// telemetry_computed row with typed columns — reconstruct payload shape
// so downstream code (DashboardPage, VCDashboardCard) can use payload.ch0_V etc.
interface ComputedRow {
  id: number
  device_key: string
  recorded_at: string
  ch0_v: number | null; ch0_i: number | null; ch0_p: number | null
  ch1_v: number | null; ch1_i: number | null; ch1_p: number | null
  ch2_v: number | null; ch2_i: number | null; ch2_p: number | null
  ch3_v: number | null; ch3_i: number | null; ch3_p: number | null
  // Additional fields from telemetry_computed that ChannelsPage reads
  ina3221_v0: number | null; ina3221_v1: number | null; ina3221_v2: number | null
  ina226_v: number | null; ina226_i: number | null; ina226_p: number | null
  ads1115_0: number | null; ads1115_1: number | null; ads1115_2: number | null; ads1115_3: number | null
  energy_wh0: number | null; energy_wh1: number | null; energy_wh2: number | null; energy_wh3: number | null
  soc_pct0: number | null; soc_pct1: number | null; soc_pct2: number | null; soc_pct3: number | null
  ina3221_i0_spike: boolean | null; ina3221_i1_spike: boolean | null; ina3221_i2_spike: boolean | null
  ina3221_v0_spike: boolean | null; ina3221_v1_spike: boolean | null; ina3221_v2_spike: boolean | null
}

function computedToPayload(row: ComputedRow): Record<string, number> {
  const p: Record<string, number> = {}
  if (row.ch0_v != null) p['ch0_V'] = row.ch0_v
  if (row.ch0_i != null) p['ch0_I'] = row.ch0_i
  if (row.ch0_p != null) p['ch0_P'] = row.ch0_p
  if (row.ch1_v != null) p['ch1_V'] = row.ch1_v
  if (row.ch1_i != null) p['ch1_I'] = row.ch1_i
  if (row.ch1_p != null) p['ch1_P'] = row.ch1_p
  if (row.ch2_v != null) p['ch2_V'] = row.ch2_v
  if (row.ch2_i != null) p['ch2_I'] = row.ch2_i
  if (row.ch2_p != null) p['ch2_P'] = row.ch2_p
  if (row.ch3_v != null) p['ch3_V'] = row.ch3_v
  if (row.ch3_i != null) p['ch3_I'] = row.ch3_i
  if (row.ch3_p != null) p['ch3_P'] = row.ch3_p
  if (row.ina3221_v0 != null) p['ina3221_v0'] = row.ina3221_v0
  if (row.ina3221_v1 != null) p['ina3221_v1'] = row.ina3221_v1
  if (row.ina3221_v2 != null) p['ina3221_v2'] = row.ina3221_v2
  if (row.ina226_v != null) p['ina226_v'] = row.ina226_v
  if (row.ina226_i != null) p['ina226_i'] = row.ina226_i
  if (row.ina226_p != null) p['ina226_p'] = row.ina226_p
  if (row.ads1115_0 != null) p['ads1115_0'] = row.ads1115_0
  if (row.ads1115_1 != null) p['ads1115_1'] = row.ads1115_1
  if (row.ads1115_2 != null) p['ads1115_2'] = row.ads1115_2
  if (row.ads1115_3 != null) p['ads1115_3'] = row.ads1115_3
  if (row.energy_wh0 != null) p['energy_wh0'] = row.energy_wh0
  if (row.energy_wh1 != null) p['energy_wh1'] = row.energy_wh1
  if (row.energy_wh2 != null) p['energy_wh2'] = row.energy_wh2
  if (row.energy_wh3 != null) p['energy_wh3'] = row.energy_wh3
  if (row.soc_pct0 != null) p['soc_pct0'] = row.soc_pct0
  if (row.soc_pct1 != null) p['soc_pct1'] = row.soc_pct1
  if (row.soc_pct2 != null) p['soc_pct2'] = row.soc_pct2
  if (row.soc_pct3 != null) p['soc_pct3'] = row.soc_pct3
  if (row.ina3221_v0 != null) p['ina3221_v0'] = row.ina3221_v0
  if (row.ina3221_v1 != null) p['ina3221_v1'] = row.ina3221_v1
  if (row.ina3221_v2 != null) p['ina3221_v2'] = row.ina3221_v2
  if (row.ina226_v != null) p['ina226_v'] = row.ina226_v
  if (row.ina226_i != null) p['ina226_i'] = row.ina226_i
  if (row.ina226_p != null) p['ina226_p'] = row.ina226_p
  if (row.ads1115_0 != null) p['ads1115_0'] = row.ads1115_0
  if (row.ads1115_1 != null) p['ads1115_1'] = row.ads1115_1
  if (row.ads1115_2 != null) p['ads1115_2'] = row.ads1115_2
  if (row.ads1115_3 != null) p['ads1115_3'] = row.ads1115_3
  if (row.ina3221_i0_spike != null) p['ina3221_i0_spike'] = row.ina3221_i0_spike ? 1 : 0
  if (row.ina3221_i1_spike != null) p['ina3221_i1_spike'] = row.ina3221_i1_spike ? 1 : 0
  if (row.ina3221_i2_spike != null) p['ina3221_i2_spike'] = row.ina3221_i2_spike ? 1 : 0
  if (row.ina3221_v0_spike != null) p['ina3221_v0_spike'] = row.ina3221_v0_spike ? 1 : 0
  if (row.ina3221_v1_spike != null) p['ina3221_v1_spike'] = row.ina3221_v1_spike ? 1 : 0
  if (row.ina3221_v2_spike != null) p['ina3221_v2_spike'] = row.ina3221_v2_spike ? 1 : 0
  return p
}

export function useRealtime(deviceKey: string | null) {
  const [dataPoints, setDataPoints] = useState<TelemetryPoint[]>([])
  const [latestReading, setLatestReading] = useState<TelemetryPoint | null>(null)
  const channelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)

  useEffect(() => {
    if (!deviceKey) return
    if (channelRef.current) {
      supabase.removeChannel(channelRef.current)
      channelRef.current = null
    }
    setLatestReading(null)
    setDataPoints([])

    // telemetry_live deleted immediately by trigger — subscribe to telemetry_computed
    const channel = supabase
      .channel(`telemetry-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_computed',
        filter: `device_key=eq.${deviceKey}`,
      }, payload => {
        const row = payload.new as ComputedRow
        const point: TelemetryPoint = {
          id: row.id,
          device_id: row.device_key,
          recorded_at: row.recorded_at,
          payload: computedToPayload(row),
          metadata: {},
        }
        setDataPoints(prev => [...prev.slice(-HISTORY_LIMIT), point])
        setLatestReading(point)
      })
      .subscribe()

    channelRef.current = channel
    return () => {
      if (channelRef.current) {
        supabase.removeChannel(channelRef.current)
        channelRef.current = null
      }
    }
  }, [deviceKey])

  return { dataPoints, latestReading }
}