import { atom } from 'jotai'
import { atomFamily } from 'jotai/utils'
import { supabase } from '../lib/supabase'
import { refreshTriggerAtom } from './atoms'
import type { TelemetryPoint } from '../lib/types'

export type HistoryRange = '1h' | '6h' | '24h' | '7d' | '30d'
export type HistoryMetric = 'power' | 'voltage' | 'current'

export const RANGE_HOURS: Record<HistoryRange, number> = {
  '1h': 1, '6h': 6, '24h': 24, '7d': 168, '30d': 720,
}

// Supabase REST enforces a hard 1000-row limit per request.
// For ranges that need more, we paginate with .range().
const PAGE_SIZE = 1000
const RANGE_LIMITS: Record<HistoryRange, number> = {
  '1h': 4000, '6h': 1000, '24h': 20000, '7d': 50000, '30d': 50000,
}

// --- Streaming state per query ---

const PAGE_SIZE = 1000

async function fetchAllPages(
  query: any,
  limit: number,
  onBatch?: (batch: any[]) => void,
): Promise<any[]> {
  const rows: any[] = []
  const pages = Math.ceil(limit / PAGE_SIZE)
  for (let i = 0; i < pages; i++) {
    const from = i * PAGE_SIZE
    const to = from + PAGE_SIZE - 1
    const { data, error } = await query.range(from, to)
    if (error) throw error
    if (!data || data.length === 0) break
    rows.push(...data)
    if (onBatch) onBatch(data)
    if (data.length < PAGE_SIZE) break
  }
  return rows
}

// --- Streaming state per query ---

export interface HistoryStreamState {
  data: TelemetryPoint[]
  loading: boolean
  error: string | null
}

const initialStreamState: HistoryStreamState = { data: [], loading: false, error: null }

// Writable atom that accumulates data incrementally as pages arrive.
export const historyStreamAtomFamily = atomFamily(
  (_k: HistoryKey) => atom<HistoryStreamState>(initialStreamState),
  (a: HistoryKey, b: HistoryKey) => a.deviceKey === b.deviceKey && a.range === b.range && a.metric === b.metric,
)

// Trigger atom: starts fetching and appends to historyStreamAtomFamily per page.
export const startHistoryStreamAtom = atom(null, async (get, set, k: HistoryKey) => {
  const key = k
  set(historyStreamAtomFamily(key), { data: [], loading: true, error: null })

  try {
    const hours = RANGE_HOURS[k.range]

    if (k.range === '1h') {
      const since = new Date(Date.now() - hours * 3600 * 1000).toISOString()
      const base = supabase
        .from('telemetry_computed')
        .select('*')
        .eq('device_key', k.deviceKey)
        .gte('recorded_at', since)
        .order('recorded_at', { ascending: true })
      await fetchAllPages(base, RANGE_LIMITS[k.range], (page) => {
        const points = page.map((row: any): TelemetryPoint => ({
          id: row.id,
          device_id: row.device_key,
          recorded_at: row.recorded_at,
          payload: reconstructPayload(row),
          metadata: {},
        }))
        const prev = get(historyStreamAtomFamily(key))
        set(historyStreamAtomFamily(key), { ...prev, data: [...prev.data, ...points] })
      })
    } else {
      const { data, error } = await supabase.rpc('get_aggregated_telemetry', {
        p_device_key: k.deviceKey,
        p_hours: hours,
        p_metric: k.metric,
      })
      if (error) {
        console.error('get_aggregated_telemetry failed, falling back to raw paginated query', error)
        const since = new Date(Date.now() - hours * 3600 * 1000).toISOString()
        const base = supabase
          .from('telemetry_computed')
          .select('*')
          .eq('device_key', k.deviceKey)
          .gte('recorded_at', since)
          .order('recorded_at', { ascending: true })
        await fetchAllPages(base, RANGE_LIMITS[k.range] ?? 5000, (page) => {
          const points = page.map((row: any): TelemetryPoint => ({
            id: row.id,
            device_id: row.device_key,
            recorded_at: row.recorded_at,
            payload: reconstructPayload(row),
            metadata: {},
          }))
          const prev = get(historyStreamAtomFamily(key))
          set(historyStreamAtomFamily(key), { ...prev, data: [...prev.data, ...points] })
        })
      } else {
        const points = (data ?? []).map((row: any): TelemetryPoint => {
          const payload: Record<string, number> = {}
          for (const [key, val] of Object.entries(row)) {
            if (key === 'bucket') continue
            if (val != null && typeof val === 'number') payload[key] = val
          }
          return { id: 0, device_id: k.deviceKey, recorded_at: row.bucket as string, payload, metadata: {} }
        })
        const prev = get(historyStreamAtomFamily(key))
        set(historyStreamAtomFamily(key), { ...prev, data: [...prev.data, ...points] })
      }
    }

    set(historyStreamAtomFamily(key), (prev) => ({ ...prev, loading: false }))
  } catch (err) {
    set(historyStreamAtomFamily(key), { data: [], loading: false, error: String(err) })
  }
})

// --- Drilldown streaming ---

export interface DrilldownKey {
  deviceKey: string
  tStart: number
  tEnd: number
  metric: HistoryMetric
}

export const drilldownStreamAtomFamily = atomFamily(
  (_k: DrilldownKey) => atom<HistoryStreamState>(initialStreamState),
  (a: DrilldownKey, b: DrilldownKey) => a.deviceKey === b.deviceKey && a.tStart === b.tStart && a.tEnd === b.tEnd && a.metric === b.metric,
)

export const startDrilldownStreamAtom = atom(null, async (get, set, k: DrilldownKey) => {
  const key = k
  set(drilldownStreamAtomFamily(key), { data: [], loading: true, error: null })

  try {
    const base = supabase
      .from('telemetry_computed')
      .select('*')
      .eq('device_key', k.deviceKey)
      .gte('recorded_at', new Date(k.tStart).toISOString())
      .lte('recorded_at', new Date(k.tEnd).toISOString())
      .order('recorded_at', { ascending: true })
    await fetchAllPages(base, 20000, (page) => {
      const points = page.map((row: any): TelemetryPoint => ({
        id: row.id,
        device_id: row.device_key,
        recorded_at: row.recorded_at,
        payload: reconstructPayload(row),
        metadata: {},
      }))
      const prev = get(drilldownStreamAtomFamily(key))
      set(drilldownStreamAtomFamily(key), { ...prev, data: [...prev.data, ...points] })
    })
    set(drilldownStreamAtomFamily(key), (prev) => ({ ...prev, loading: false }))
  } catch (err) {
    set(drilldownStreamAtomFamily(key), { data: [], loading: false, error: String(err) })
  }
})

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