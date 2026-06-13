import { atom } from 'jotai'
import { atomFamily } from 'jotai/utils'
import { loadable } from 'jotai/utils'
import { supabase } from '../lib/supabase'
import { refreshTriggerAtom } from './atoms'
import type { TelemetryPoint } from '../lib/types'

export type HistoryRange = '1h' | '6h' | '24h' | '7d' | '30d'
export type HistoryMetric = 'power' | 'voltage' | 'current'

export const RANGE_HOURS: Record<HistoryRange, number> = {
  '1h': 1, '6h': 6, '24h': 24, '7d': 168, '30d': 720,
}
const RANGE_LIMITS: Record<HistoryRange, number> = {
  // 1h keeps raw 1-sec data; raise limit to cover the full hour.
  '1h': 4000, '6h': 1000, '24h': 20000, '7d': 50000, '30d': 50000,
}

interface HistoryKey {
  deviceKey: string
  range: HistoryRange
  metric: HistoryMetric
}

export const historyAtomFamily: any = atomFamily(
  (k: HistoryKey) => loadable(historyFetcher(k)),
  (a: HistoryKey, b: HistoryKey) => a.deviceKey === b.deviceKey && a.range === b.range && a.metric === b.metric,
)

function historyFetcher(k: HistoryKey) {
  return atom(async (get) => {
    get(refreshTriggerAtom)
    const hours = RANGE_HOURS[k.range]
    if (k.range === '1h') {
      const since = new Date(Date.now() - hours * 3600 * 1000).toISOString()
      const { data, error } = await supabase
        .from('telemetry_computed')
        .select('*')
        .eq('device_key', k.deviceKey)
        .gte('recorded_at', since)
        .order('recorded_at', { ascending: true })
        .limit(RANGE_LIMITS[k.range])
      if (error) throw error
      return (data ?? []).map((row: any): TelemetryPoint => ({
        id: row.id,
        device_id: row.device_key,
        recorded_at: row.recorded_at,
        payload: reconstructPayload(row),
        metadata: {},
      }))
    }
    const { data, error } = await supabase.rpc('get_aggregated_telemetry', {
      p_device_key: k.deviceKey,
      p_hours: hours,
      p_metric: k.metric,
    })
    if (error) {
      console.error('get_aggregated_telemetry failed', error)
      return []
    }
    return (data ?? []).map((row: any): TelemetryPoint => {
      const payload: Record<string, number> = {}
      for (const [key, val] of Object.entries(row)) {
        if (key === 'bucket') continue
        if (val != null && typeof val === 'number') payload[key] = val
      }
      return { id: 0, device_id: k.deviceKey, recorded_at: row.bucket as string, payload, metadata: {} }
    })
  })
}

interface DrilldownKey {
  deviceKey: string
  tStart: number
  tEnd: number
  metric: HistoryMetric
}

export const drilldownLoadableAtom: any = atomFamily(
  (k: DrilldownKey) => loadable(drilldownFetcher(k)),
  (a: DrilldownKey, b: DrilldownKey) => a.deviceKey === b.deviceKey && a.tStart === b.tStart && a.tEnd === b.tEnd && a.metric === b.metric,
)

function drilldownFetcher(k: DrilldownKey) {
  return atom(async (get) => {
    get(refreshTriggerAtom)
    const { data, error } = await supabase
      .from('telemetry_computed')
      .select('*')
      .eq('device_key', k.deviceKey)
      .gte('recorded_at', new Date(k.tStart).toISOString())
      .lte('recorded_at', new Date(k.tEnd).toISOString())
      .order('recorded_at', { ascending: true })
      .limit(20000)
    if (error) throw error
    return (data ?? []).map((row: any): TelemetryPoint => ({
      id: row.id,
      device_id: row.device_key,
      recorded_at: row.recorded_at,
      payload: reconstructPayload(row),
      metadata: {},
    }))
  })
}

function reconstructPayload(row: any): Record<string, number> {
  const p: Record<string, number> = {}
  const set = (k: string, v: any) => { if (v != null && typeof v === 'number') p[k] = v }
  // Uppercase keys for the 1h/6h typed-column path, matching the original
  // chart's key shape. The 7d/30d RPC returns lowercase keys directly.
  set('ch0_V', row.ch0_v); set('ch0_I', row.ch0_i); set('ch0_P', row.ch0_p)
  set('ch1_V', row.ch1_v); set('ch1_I', row.ch1_i); set('ch1_P', row.ch1_p)
  set('ch2_V', row.ch2_v); set('ch2_I', row.ch2_i); set('ch2_P', row.ch2_p)
  set('ch3_V', row.ch3_v); set('ch3_I', row.ch3_i); set('ch3_P', row.ch3_p)
  set('pv_power', row.pv_power)
  set('battery_power', row.battery_power)
  set('inverter_power', row.inverter_power)
  set('dc_load_power', row.dc_load_power)
  set('inverter_current', row.inverter_current)
  set('energy_wh0', row.energy_wh0); set('energy_wh1', row.energy_wh1)
  set('energy_wh2', row.energy_wh2); set('energy_wh3', row.energy_wh3)
  set('soc_pct0', row.soc_pct0); set('soc_pct1', row.soc_pct1)
  set('soc_pct2', row.soc_pct2); set('soc_pct3', row.soc_pct3)
  return p
}