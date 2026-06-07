import { atom } from 'jotai'
import { atomFamily } from 'jotai/utils'
import { latestAtom, nowAtom } from './atoms'
import { computeTelemetry, type ComputedValues } from '../lib/computedTelemetry'
import type { ChannelGroup, BatteryProfile } from '../lib/types'

const LIVE_BUFFER_LIMIT = 200

export const liveBufferAtom = atom((get) => {
  const latest = get(latestAtom)
  if (!latest) return []
  return [latest] // future: extend when service writes to a buffer atom
})

export const computedTelemetryAtom = atom<ComputedValues>((get) => {
  const latest = get(latestAtom)
  const payload = (latest?.payload ?? {}) as Record<string, number>
  return computeTelemetry(payload, [] as ChannelGroup[], [] as BatteryProfile[])
})

export interface ChannelPayload {
  voltage: number | null
  current: number | null
  power: number | null
  energyWh: number | null
  socPct: number | null
}

export const channelPayloadAtomFamily = atomFamily((channelIdx: number) =>
  atom<ChannelPayload>((get) => {
    const latest = get(latestAtom)
    const p = (latest?.payload ?? {}) as Record<string, number>
    return {
      voltage: p[`ch${channelIdx}_V`] ?? null,
      current: p[`ch${channelIdx}_I`] ?? null,
      power: p[`ch${channelIdx}_P`] ?? null,
      energyWh: p[`energy_wh${channelIdx}`] ?? null,
      socPct: p[`soc_pct${channelIdx}`] ?? null,
    }
  }),
)

export const secondsAgoAtom = atom<number | null>((get) => {
  const latest = get(latestAtom)
  if (!latest) return null
  get(nowAtom) // subscribe to now changes
  return Math.max(0, Math.floor((Date.now() - new Date(latest.recorded_at).getTime()) / 1000))
})

// Convenience derivations (widget-level)
export const inverterPowerAtom = atom((get) => get(computedTelemetryAtom).inverter_power)
export const pvPowerAtom = atom((get) => get(computedTelemetryAtom).pv_power)
export const batteryPowerAtom = atom((get) => get(computedTelemetryAtom).battery_power)
export const systemStatusAtom = atom((get) => get(computedTelemetryAtom).system_status)