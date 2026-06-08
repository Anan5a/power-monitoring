import { describe, it, expect } from 'vitest'
import { buildChannelLabelMap, keyToLabel } from '../historyService'
import type { ChannelGroup } from '../../../lib/types'

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
})
