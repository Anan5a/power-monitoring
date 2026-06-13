import type { Setter } from 'jotai'
import type { Store } from 'jotai/vanilla/store'
import { supabase } from '../../lib/supabase'
import {
  latestAtom,
  liveBufferAtomPrimitive,
  appendBufferAtom,
  connectionStateAtom,
} from '../atoms'
import type { TelemetryPoint } from '../../lib/types'

interface TelemetryRow {
  id: number
  device_key: string
  recorded_at: string
  ch0_v: number | null; ch0_i: number | null; ch0_p: number | null
  ch1_v: number | null; ch1_i: number | null; ch1_p: number | null
  ch2_v: number | null; ch2_i: number | null; ch2_p: number | null
  ch3_v: number | null; ch3_i: number | null; ch3_p: number | null
  // server-computed values
  pv_power: number | null
  battery_power: number | null
  inverter_power: number | null
  dc_load_power: number | null
  system_status: 'unknown' | 'charging' | 'discharging' | 'balanced' | null
  energy_wh0: number | null; energy_wh1: number | null
  energy_wh2: number | null; energy_wh3: number | null
  soc_pct0: number | null; soc_pct1: number | null
  soc_pct2: number | null; soc_pct3: number | null
  // raw sensor values
  ina3221_v0: number | null; ina3221_v1: number | null; ina3221_v2: number | null
  ina3221_i0: number | null; ina3221_i1: number | null; ina3221_i2: number | null
  ina226_v: number | null; ina226_i: number | null; ina226_p: number | null
  ads1115_0: number | null; ads1115_1: number | null; ads1115_2: number | null; ads1115_3: number | null
}

function rowToPoint(row: TelemetryRow): TelemetryPoint {
  const p: Record<string, number> = {}
  const set = (k: string, v: number | null | undefined) => {
    if (v != null) p[k] = v
  }
  // Virtual channels
  set('ch0_V', row.ch0_v); set('ch0_I', row.ch0_i); set('ch0_P', row.ch0_p)
  set('ch1_V', row.ch1_v); set('ch1_I', row.ch1_i); set('ch1_P', row.ch1_p)
  set('ch2_V', row.ch2_v); set('ch2_I', row.ch2_i); set('ch2_P', row.ch2_p)
  set('ch3_V', row.ch3_v); set('ch3_I', row.ch3_i); set('ch3_P', row.ch3_p)
  // System computed values
  set('pv_power', row.pv_power)
  set('battery_power', row.battery_power)
  set('inverter_power', row.inverter_power)
  set('dc_load_power', row.dc_load_power)
  // Energy / SoC
  set('energy_wh0', row.energy_wh0); set('energy_wh1', row.energy_wh1)
  set('energy_wh2', row.energy_wh2); set('energy_wh3', row.energy_wh3)
  set('soc_pct0', row.soc_pct0); set('soc_pct1', row.soc_pct1)
  set('soc_pct2', row.soc_pct2); set('soc_pct3', row.soc_pct3)
  // Raw sensor values (used by Channels page)
  set('ina3221_v0', row.ina3221_v0); set('ina3221_v1', row.ina3221_v1); set('ina3221_v2', row.ina3221_v2)
  set('ina3221_i0', row.ina3221_i0); set('ina3221_i1', row.ina3221_i1); set('ina3221_i2', row.ina3221_i2)
  set('ina226_v', row.ina226_v); set('ina226_i', row.ina226_i); set('ina226_p', row.ina226_p)
  set('ads1115_0', row.ads1115_0); set('ads1115_1', row.ads1115_1)
  set('ads1115_2', row.ads1115_2); set('ads1115_3', row.ads1115_3)
  if (row.system_status) p['system_status'] = row.system_status as unknown as number
  return {
    id: row.id,
    device_id: row.device_key,
    recorded_at: row.recorded_at,
    payload: p,
    metadata: {},
  }
}

const STALE_MS = 15_000
let currentChannel: ReturnType<typeof supabase.channel> | null = null
let staleTimer: ReturnType<typeof setTimeout> | null = null
let visibilityHandler: (() => void) | null = null
let activeStore: Store | null = null
let activeDeviceKey: string | null = null

function clearStaleTimer() {
  if (staleTimer) { clearTimeout(staleTimer); staleTimer = null }
}

function armStaleTimer(set: Setter) {
  clearStaleTimer()
  staleTimer = setTimeout(() => {
    set(connectionStateAtom, 'stale')
  }, STALE_MS)
}

export function startLiveTelemetry(store: Store, deviceKey: string) {
  stopLiveTelemetry()
  activeStore = store
  activeDeviceKey = deviceKey
  store.set(latestAtom, null)
  store.set(liveBufferAtomPrimitive, [])
  store.set(connectionStateAtom, 'connecting')

  const channel = supabase
    .channel(`telemetry-${deviceKey}`)
    .on('postgres_changes', {
      event: 'INSERT',
      schema: 'public',
      table: 'telemetry_computed',
      filter: `device_key=eq.${deviceKey}`,
    }, (payload) => {
      const row = payload.new as TelemetryRow
      const point = rowToPoint(row)
      store.set(latestAtom, point)
      store.set(appendBufferAtom, point)
      store.set(connectionStateAtom, 'live')
      armStaleTimer(store.set)
    })
    .subscribe()
  currentChannel = channel

  // visibility-pause: don't re-arm the stale timer while tab is hidden
  visibilityHandler = () => {
    if (document.visibilityState === 'hidden') {
      clearStaleTimer()
    } else if (activeStore && activeDeviceKey) {
      store.set(connectionStateAtom, 'connecting')
      armStaleTimer(store.set)
    }
  }
  document.addEventListener('visibilitychange', visibilityHandler)
  armStaleTimer(store.set)
}

export function stopLiveTelemetry() {
  clearStaleTimer()
  if (visibilityHandler) {
    document.removeEventListener('visibilitychange', visibilityHandler)
    visibilityHandler = null
  }
  if (currentChannel) {
    supabase.removeChannel(currentChannel)
    currentChannel = null
  }
  activeStore = null
  activeDeviceKey = null
}