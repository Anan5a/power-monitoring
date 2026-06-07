import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { createStore } from 'jotai'

// Mock supabase BEFORE importing the service
vi.mock('../../../lib/supabase', () => {
  const handlers: Array<(payload: any) => void> = []
  const channelObj = {
    on: (_evt: string, _cfg: unknown, h: (p: any) => void) => {
      handlers.push(h)
      return channelObj
    },
    subscribe: () => ({}),
  }
  return {
    supabase: {
      channel: () => channelObj,
      removeChannel: () => {},
      __handlers: handlers,
      __fire: (payload: any) => handlers.forEach(h => h(payload)),
    },
  }
})

import { startLiveTelemetry, stopLiveTelemetry } from '../telemetryService'
import { latestAtom, liveBufferAtomPrimitive, connectionStateAtom } from '../../atoms'
import * as supa from '../../../lib/supabase'

describe('telemetryService', () => {
  let store: ReturnType<typeof createStore>

  beforeEach(() => {
    store = createStore()
  })

  afterEach(() => {
    stopLiveTelemetry()
  })

  it('sets connectionState to live on first message', () => {
    startLiveTelemetry(store, 'dev1')
    ;(supa.supabase as any).__fire({ new: { id: 1, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    expect(store.get(connectionStateAtom)).toBe('live')
  })

  it('writes latest + appends to buffer', () => {
    startLiveTelemetry(store, 'dev1')
    ;(supa.supabase as any).__fire({ new: { id: 1, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    const latest = store.get(latestAtom)
    expect(latest).not.toBeNull()
    expect(store.get(liveBufferAtomPrimitive).length).toBe(1)
  })

  it('caps the buffer at 200', () => {
    startLiveTelemetry(store, 'dev1')
    for (let i = 0; i < 250; i++) {
      ;(supa.supabase as any).__fire({ new: { id: i, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    }
    expect(store.get(liveBufferAtomPrimitive).length).toBe(200)
    expect(store.get(liveBufferAtomPrimitive)[0].id).toBe(50)
  })

  it('marks stale after 15s of no messages', async () => {
    vi.useFakeTimers()
    startLiveTelemetry(store, 'dev1')
    ;(supa.supabase as any).__fire({ new: { id: 1, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    expect(store.get(connectionStateAtom)).toBe('live')
    vi.advanceTimersByTime(16000)
    expect(store.get(connectionStateAtom)).toBe('stale')
    vi.useRealTimers()
  })
})