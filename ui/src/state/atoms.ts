import { atom } from 'jotai'
import { atomFamily, atomWithReducer } from 'jotai/utils'
import type { TelemetryPoint, DeviceChannels, RelayState, LayoutDoc } from '../lib/types'

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