import { atom } from 'jotai'
import { atomFamily, atomWithReducer } from 'jotai/utils'
import type { TelemetryPoint, DeviceChannels, RelayState, LayoutDoc, Device } from '../lib/types'

// --- Connection / time ---

export const connectionStateAtom = atom<
  'connecting' | 'live' | 'stale' | 'offline'
>('offline')

// Updated once per second by useNowTicker (mounted in App.tsx)
export const nowAtom = atomWithReducer<number, void>(
  Date.now(),
  () => Date.now(),
)

// --- Telemetry (set by telemetryService) ---

export const latestAtom = atom<TelemetryPoint | null>(null)

// Ring buffer of recent points, capped at 200. Service writes via appendBufferAtom.
export const liveBufferAtomPrimitive = atom<TelemetryPoint[]>([])
export const appendBufferAtom = atom(
  null,
  (get, set, point: TelemetryPoint) => {
    const next = [...get(liveBufferAtomPrimitive), point]
    if (next.length > 200) next.splice(0, next.length - 200)
    set(liveBufferAtomPrimitive, next)
  },
)

// Bumped to invalidate history loadables
export const refreshTriggerAtom = atom(0)

// --- Per-device data (atomFamily: one entry per deviceKey) ---

export const deviceChannelsAtomFamily = atomFamily((_deviceKey: string) =>
  atom<DeviceChannels | null>(null),
)

export const relayStatesAtomFamily = atomFamily((_deviceKey: string) =>
  atom<RelayState[]>([]),
)

// --- Layout ---

export const layoutAtom = atom<LayoutDoc | null>(null)

// --- Chart zoom + drilldown ---

export interface ZoomRange { start: number; end: number }
export const zoomRangeAtom = atom<ZoomRange | null>(null)

export interface BreadcrumbEntry { rangeLabel: string; tStart: number; tEnd: number; fromRange: string }
export const drilldownBreadcrumbAtom = atom<BreadcrumbEntry[]>([])

export const hoveredPointAtom = atom<{ time: string; values: Record<string, number> } | null>(null)

// --- Selected device (global, so widgets can read it) ---

export const selectedDeviceAtom = atom<Device | null>(null)

// --- Devices list (loaded once, shared across pages) ---

export const devicesAtom = atom<Device[]>([])

// --- Aggregates (RPC-backed) ---

export type GenerationRange = 'today' | 'yesterday' | '7d' | '30d'

export interface GenerationData {
  total: number
  hourly: Array<{ hour: string; value: number; projected?: boolean }>
  rangeLabel: string
}

export const generationRangeAtom = atom<GenerationRange>('today')

export interface BatteryData {
  chargeWh: number
  capacityWh: number
  energyIn24h: number
  energyOut24h: number
  isFullChargeToday: boolean
}

export const generationDataAtom = atom<GenerationData | null>(null)
export const batteryDataAtom = atom<BatteryData | null>(null)
export const batteryLoadingAtom = atom<boolean>(true)
export const generationLoadingAtom = atom<boolean>(false)