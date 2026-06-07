import { describe, it, expect } from 'vitest'
import { createStore } from 'jotai'
import {
  latestAtom,
  nowAtom,
} from '../atoms'
import {
  liveBufferAtom,
  computedTelemetryAtom,
  channelPayloadAtomFamily,
  secondsAgoAtom,
} from '../derived'

function makePoint(secondsAgo: number, payload: Record<string, number>) {
  return {
    id: 1,
    device_id: 'k',
    recorded_at: new Date(Date.now() - secondsAgo * 1000).toISOString(),
    payload,
    metadata: {},
  }
}

describe('liveBufferAtom', () => {
  it('is empty when no latest', () => {
    const store = createStore()
    expect(store.get(liveBufferAtom)).toEqual([])
  })

  it('contains the latest when set', () => {
    const store = createStore()
    const pt = makePoint(0, { ch0_P: 10 })
    store.set(latestAtom, pt)
    expect(store.get(liveBufferAtom)).toEqual([pt])
  })
})

describe('computedTelemetryAtom', () => {
  it('returns zeros when payload is empty', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(0, {}))
    const c = store.get(computedTelemetryAtom)
    expect(c.pv_power).toBe(0)
    expect(c.inverter_power).toBe(0)
    expect(c.system_status).toBe('balanced')
  })
})

describe('channelPayloadAtomFamily', () => {
  it('returns nulls for missing channel', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(0, { ch0_V: 12.3 }))
    const p = store.get(channelPayloadAtomFamily(2))
    expect(p.voltage).toBeNull()
    expect(p.current).toBeNull()
  })

  it('returns values for present channel', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(0, { ch2_V: 13.5, ch2_I: 2.1, ch2_P: 28.4, energy_wh2: 100, soc_pct2: 75 }))
    const p = store.get(channelPayloadAtomFamily(2))
    expect(p.voltage).toBe(13.5)
    expect(p.current).toBe(2.1)
    expect(p.power).toBe(28.4)
    expect(p.energyWh).toBe(100)
    expect(p.socPct).toBe(75)
  })
})

describe('secondsAgoAtom', () => {
  it('returns null when no latest', () => {
    const store = createStore()
    expect(store.get(secondsAgoAtom)).toBeNull()
  })

  it('returns the delta in seconds', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(7, {}))
    store.set(nowAtom) // triggers reducer to update now
    const s = store.get(secondsAgoAtom)
    expect(s).toBeGreaterThanOrEqual(7)
    expect(s).toBeLessThan(8)
  })
})