import { atom } from 'jotai'
import { atomFamily } from 'jotai/utils'
import { latestAtom, nowAtom, liveBufferAtomPrimitive, selectedDeviceAtom, deviceChannelsAtomFamily } from './atoms'
import { computeTelemetry, type ComputedValues } from '../lib/computedTelemetry'
import type { ChannelGroup, BatteryProfile } from '../lib/types'

export const liveBufferAtom = atom((get) => get(liveBufferAtomPrimitive))

export const computedTelemetryAtom = atom<ComputedValues>((get) => {
  const latest = get(latestAtom)
  const device = get(selectedDeviceAtom)
  const channels = device ? get(deviceChannelsAtomFamily(device.device_key)) : null
  const payload = (latest?.payload ?? {}) as Record<string, number>
  return computeTelemetry(
    payload,
    (channels?.channel_groups ?? []) as ChannelGroup[],
    (channels?.battery_profiles ?? []) as BatteryProfile[],
  )
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
  const _tick = get(nowAtom) // subscribe to now changes; must use the value
  void _tick
  return Math.max(0, Math.floor((Date.now() - new Date(latest.recorded_at).getTime()) / 1000))
})

// Convenience derivations (widget-level)
export const inverterPowerAtom = atom((get) => get(computedTelemetryAtom).inverter_power)
export const pvPowerAtom = atom((get) => get(computedTelemetryAtom).pv_power)
export const batteryPowerAtom = atom((get) => get(computedTelemetryAtom).battery_power)
export const systemStatusAtom = atom((get) => get(computedTelemetryAtom).system_status)