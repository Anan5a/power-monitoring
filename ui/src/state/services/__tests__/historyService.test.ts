import { describe, it, expect } from 'vitest'
import { buildChannelLabelMap, keyToLabel, computeSystemCurrents, withSystemCurrents, extractKeys } from '../historyService'
import type { ChannelGroup, TelemetryPoint } from '../../../lib/types'

describe('buildChannelLabelMap', () => {
  it('returns empty map for null/undefined input', () => {
    expect(buildChannelLabelMap(undefined).size).toBe(0)
    expect(buildChannelLabelMap(null).size).toBe(0)
    expect(buildChannelLabelMap([]).size).toBe(0)
  })

  it('uses group.name when present (matches system tab labels)', () => {
    // Mirrors the user's device: two PV groups with distinct names.
    const groups: ChannelGroup[] = [
      { group_id: 0, name: 'PV Voltage', icon: 0, channel_mask: 1 },
      { group_id: 1, name: 'PV Power',   icon: 0, channel_mask: 2 },
      { group_id: 2, name: 'Battery',    icon: 1, channel_mask: 4 },
      { group_id: 3, name: 'DC Power',   icon: 3, channel_mask: 8 },
    ]
    const map = buildChannelLabelMap(groups)
    expect(map.get(0)).toBe('PV Voltage')
    expect(map.get(1)).toBe('PV Power')
    expect(map.get(2)).toBe('Battery')
    expect(map.get(3)).toBe('DC Power')
  })

  it('falls back to icon-based name when group.name is missing', () => {
    const groups: ChannelGroup[] = [
      { group_id: 0, name: '', icon: 0, channel_mask: 1 },
      { group_id: 2, name: '', icon: 1, channel_mask: 4 },
    ]
    const map = buildChannelLabelMap(groups)
    expect(map.get(0)).toBe('PV')
    expect(map.get(2)).toBe('Battery')
  })

  it('numbers multiple unnamed groups sharing the same icon', () => {
    const groups: ChannelGroup[] = [
      { group_id: 0, name: '', icon: 0, channel_mask: 1 },
      { group_id: 1, name: '', icon: 0, channel_mask: 2 },
    ]
    const map = buildChannelLabelMap(groups)
    expect(map.get(0)).toBe('PV 1')
    expect(map.get(1)).toBe('PV 2')
  })

  it('numbers channels within a single multi-channel group', () => {
    const groups: ChannelGroup[] = [
      { group_id: 0, name: 'PV', icon: 0, channel_mask: 3 }, // ch0 + ch1
    ]
    const map = buildChannelLabelMap(groups)
    expect(map.get(0)).toBe('PV 1')
    expect(map.get(1)).toBe('PV 2')
  })

  it('skips placeholder names like "ch0_V"', () => {
    const groups: ChannelGroup[] = [
      { group_id: 0, name: 'ch0_V', icon: 0, channel_mask: 1 },
    ]
    const map = buildChannelLabelMap(groups)
    expect(map.get(0)).toBe('PV')
  })

  it('lets the first group win when channels overlap', () => {
    const groups: ChannelGroup[] = [
      { group_id: 0, name: 'Primary',   icon: 0, channel_mask: 1 },
      { group_id: 1, name: 'Secondary', icon: 1, channel_mask: 1 },
    ]
    const map = buildChannelLabelMap(groups)
    expect(map.get(0)).toBe('Primary')
  })
})

describe('keyToLabel with channelLabelMap', () => {
  const groups: ChannelGroup[] = [
    { group_id: 0, name: 'PV Voltage', icon: 0, channel_mask: 1 },
    { group_id: 1, name: 'PV Power',   icon: 0, channel_mask: 2 },
    { group_id: 2, name: 'Battery',    icon: 1, channel_mask: 4 },
  ]
  const map = buildChannelLabelMap(groups)

  it('renders raw channel keys with friendly names', () => {
    expect(keyToLabel('ch0_V', undefined, map)).toBe('PV Voltage V')
    expect(keyToLabel('ch1_P', undefined, map)).toBe('PV Power P')
    expect(keyToLabel('ch2_I', undefined, map)).toBe('Battery I')
  })

  it('keeps system power key labels', () => {
    expect(keyToLabel('pv_power')).toBe('PV Generation')
    expect(keyToLabel('battery_power')).toBe('Battery')
    expect(keyToLabel('inverter_power')).toBe('Inverter Output')
  })

  it('falls back to VC{n} when no map and no channel_names', () => {
    expect(keyToLabel('ch3_P', undefined, new Map())).toBe('VC3 P')
  })

  it('uses channel_names override as second priority', () => {
    expect(keyToLabel('ch0_P', [{ channel: 0, name: 'Solar' }])).toBe('Solar P')
  })

  it('labels the new system current keys', () => {
    expect(keyToLabel('pv_current')).toBe('PV Current')
    expect(keyToLabel('battery_current')).toBe('Battery Current')
    expect(keyToLabel('dc_load_current')).toBe('DC Load Current')
  })
})

describe('computeSystemCurrents', () => {
  const groups: ChannelGroup[] = [
    { group_id: 0, name: 'PV Voltage', icon: 0, channel_mask: 1 },  // ch0
    { group_id: 1, name: 'PV Power',   icon: 0, channel_mask: 2 },  // ch1
    { group_id: 2, name: 'Battery',    icon: 1, channel_mask: 4 },  // ch2
    { group_id: 3, name: 'DC Power',   icon: 3, channel_mask: 8 },  // ch3
  ]

  it('returns empty object when no groups', () => {
    expect(computeSystemCurrents({ ch0_I: 5 }, undefined)).toEqual({})
    expect(computeSystemCurrents({ ch0_I: 5 }, [])).toEqual({})
  })

  it('sums |ch_i| per group icon (PV combines both PV groups)', () => {
    const payload = { ch0_I: 1.5, ch1_I: 2.0, ch2_I: 0.3, ch3_I: 4.0 }
    const out = computeSystemCurrents(payload, groups)
    expect(out.pv_current).toBeCloseTo(3.5)        // 1.5 + 2.0
    expect(out.battery_current).toBeCloseTo(0.3)
    expect(out.dc_load_current).toBeCloseTo(4.0)
  })

  it('preserves the sign on battery_current so charging (+) and discharging (-) are distinguishable', () => {
    const charging = computeSystemCurrents({ ch2_I: 1.2 }, groups)
    expect(charging.battery_current).toBeCloseTo(1.2)        // + = charging
    const discharging = computeSystemCurrents({ ch2_I: -0.8 }, groups)
    expect(discharging.battery_current).toBeCloseTo(-0.8)    // - = discharging
  })

  it('passes inverter_current through with its sign intact', () => {
    const out = computeSystemCurrents({ ch0_I: 1.0, inverter_current: -3.5 }, groups)
    expect(out.inverter_current).toBe(-3.5)
  })

  it('handles uppercase and lowercase keys', () => {
    const out = computeSystemCurrents({ ch0_i: 1.0, ch2_i: 2.0 }, groups)
    expect(out.pv_current).toBeCloseTo(1.0)
    expect(out.battery_current).toBeCloseTo(2.0)
  })

  it('passes through inverter_current when present in payload', () => {
    const out = computeSystemCurrents({ ch0_I: 1.0, inverter_current: 5.0 }, groups)
    expect(out.inverter_current).toBe(5.0)
  })

  it('omits keys when the corresponding channels have no data', () => {
    const out = computeSystemCurrents({ ch0_I: 1.0 }, groups)
    expect(out.pv_current).toBeCloseTo(1.0)
    expect(out.battery_current).toBeUndefined()
    expect(out.dc_load_current).toBeUndefined()
  })
})

describe('withSystemCurrents', () => {
  const groups: ChannelGroup[] = [
    { group_id: 0, name: 'PV', icon: 0, channel_mask: 1 },
  ]
  it('merges derived keys into the original payload', () => {
    const out = withSystemCurrents({ ch0_I: 1.5, ch0_V: 24.0 }, groups)
    expect(out.ch0_V).toBe(24.0)
    expect(out.pv_current).toBeCloseTo(1.5)
  })
  it('returns original payload unchanged when nothing can be derived', () => {
    const payload = { ch0_V: 24.0 }
    expect(withSystemCurrents(payload, groups)).toBe(payload)
  })
})

describe('extractKeys for current with system currents', () => {
  const groups: ChannelGroup[] = [
    { group_id: 0, name: 'PV',     icon: 0, channel_mask: 1 },
    { group_id: 2, name: 'Battery',icon: 1, channel_mask: 4 },
    { group_id: 3, name: 'DC',     icon: 3, channel_mask: 8 },
  ]
  const data: TelemetryPoint[] = [
    { id: 0, device_id: 'k', recorded_at: '2026-06-08T00:00:00Z',
      payload: { ch0_I: 1.5, ch2_I: 0.3, ch3_I: 4.0 }, metadata: {} },
    { id: 1, device_id: 'k', recorded_at: '2026-06-08T00:00:01Z',
      payload: { ch0_I: 1.6, ch2_I: 0.4, ch3_I: 4.1 }, metadata: {} },
  ]

  it('prepends system current keys in display order when groups are present', () => {
    const keys = extractKeys(data, 'current', groups)
    expect(keys).toContain('pv_current')
    expect(keys).toContain('battery_current')
    expect(keys).toContain('dc_load_current')
    // system keys come before raw channels
    const sysIdx = keys.indexOf('pv_current')
    const rawIdx = keys.indexOf('ch0_I')
    expect(sysIdx).toBeLessThan(rawIdx)
  })

  it('omits system current keys when channel_groups is missing', () => {
    const keys = extractKeys(data, 'current')
    expect(keys).not.toContain('pv_current')
    expect(keys).toContain('ch0_I')
  })

  it('omits system current keys when all derived values are zero', () => {
    const allZero: TelemetryPoint[] = [
      { id: 0, device_id: 'k', recorded_at: '2026-06-08T00:00:00Z',
        payload: { ch0_I: 0, ch2_I: 0, ch3_I: 0 }, metadata: {} },
    ]
    const keys = extractKeys(allZero, 'current', groups)
    expect(keys).not.toContain('pv_current')
  })
})
