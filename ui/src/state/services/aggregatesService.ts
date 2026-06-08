import { atom } from 'jotai'
import { supabase } from '../../lib/supabase'
import { selectedDeviceAtom, refreshTriggerAtom, generationRangeAtom, generationDataAtom, batteryDataAtom, generationLoadingAtom, batteryLoadingAtom, type GenerationRange, type GenerationData, type BatteryData } from '../atoms'

const POLL_BATTERY_MS = 10_000

// --- Derived atoms that drive the RPC fetches ---

// When the selected device OR the range changes, fire the generation RPC.
export const generationFetcherAtom = atom(null, async (get, set) => {
  const device = get(selectedDeviceAtom)
  const range = get(generationRangeAtom)
  if (!device) {
    set(generationDataAtom, null)
    set(generationLoadingAtom, false)
    return
  }
  set(generationLoadingAtom, true)
  const data = await fetchGeneration(device.device_key, range)
  set(generationDataAtom, data)
  set(generationLoadingAtom, false)
})

// Battery charge polls every POLL_BATTERY_MS while a device is selected.
// Re-runs on device change, manual refresh, or interval tick.
export const batteryFetcherAtom = atom(null, async (get, set) => {
  const device = get(selectedDeviceAtom)
  get(refreshTriggerAtom) // manual refresh invalidates
  if (!device) {
    set(batteryDataAtom, null)
    set(batteryLoadingAtom, false)
    return
  }
  set(batteryLoadingAtom, true)
  const data = await fetchBattery(device.id)
  set(batteryDataAtom, data)
  set(batteryLoadingAtom, false)
})

// --- Plain async helpers (no atoms) ---

function rangeToBounds(range: GenerationRange): { start: Date; end: Date; label: string; isDaily: boolean } {
  const now = new Date()
  let startTime: Date
  let endTime: Date
  let rangeLabel = ''
  let isDaily = false
  if (range === 'today') {
    startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()))
    endTime = now
    rangeLabel = 'Today'
  } else if (range === 'yesterday') {
    startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1))
    endTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1, 23, 59, 59, 999))
    rangeLabel = 'Yesterday'
  } else if (range === '7d') {
    startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 6))
    endTime = now
    isDaily = true
    rangeLabel = 'Last 7 Days'
  } else {
    startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 29))
    endTime = now
    isDaily = true
    rangeLabel = 'Last 30 Days'
  }
  return { start: startTime, end: endTime, label: rangeLabel, isDaily }
}

export async function fetchGeneration(deviceKey: string, range: GenerationRange): Promise<GenerationData | null> {
  const { start, end, label, isDaily } = rangeToBounds(range)
  const fn = isDaily ? 'get_daily_generation' : 'get_hourly_generation'
  const { data, error } = await supabase.rpc(fn, {
    p_device_key: deviceKey,
    p_start_time: start.toISOString(),
    p_end_time: end.toISOString(),
  })
  if (error || !Array.isArray(data) || data.length === 0) {
    return { total: 0, hourly: [], rangeLabel: label }
  }
  let total = 0
  const buckets = (data as any[]).map((row) => {
    const kwh = row.kwh ?? 0
    const hourLabel = isDaily
      ? `${String(new Date(row.day).getMonth() + 1).padStart(2, '0')}/${String(new Date(row.day).getDate()).padStart(2, '0')}`
      : `${String(new Date(row.hour_start).getHours()).padStart(2, '0')}:00`
    total += kwh
    return { hour: hourLabel, value: Math.round(kwh * 100) / 100, projected: row.is_partial ?? false }
  }).reverse()
  return { total: Math.round(total * 100) / 100, hourly: buckets, rangeLabel: label }
}

export async function fetchBattery(deviceId: string): Promise<BatteryData | null> {
  const { data, error } = await supabase.rpc('get_battery_charge', { p_device_id: deviceId, p_hours: 24 })
  if (error || !data) return null
  const row = Array.isArray(data) ? data[0] : data
  return {
    chargeWh: row.charge_wh ?? 0,
    capacityWh: row.capacity_wh ?? 0,
    energyIn24h: row.energy_in_24h ?? 0,
    energyOut24h: row.energy_out_24h ?? 0,
    isFullChargeToday: row.is_full_charge_today ?? false,
  }
}

// --- Lifecycle: start/stop the polling loop. Mount once in App.tsx. ---

let batteryTimer: ReturnType<typeof setInterval> | null = null

export function startAggregatesPolling(store: any) {
  stopAggregatesPolling()
  // Run generation once
  store.set(generationFetcherAtom)
  // Run battery once + poll
  store.set(batteryFetcherAtom)
  batteryTimer = setInterval(() => store.set(batteryFetcherAtom), POLL_BATTERY_MS)
}

export function stopAggregatesPolling() {
  if (batteryTimer) {
    clearInterval(batteryTimer)
    batteryTimer = null
  }
}

// Re-export for the previous imports
export { selectedDeviceAtom, refreshTriggerAtom, generationRangeAtom, generationDataAtom, batteryDataAtom, generationLoadingAtom, batteryLoadingAtom }
export type { GenerationRange, GenerationData, BatteryData }