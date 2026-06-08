import type { HistoryRange, HistoryMetric } from '../history'
import type { TelemetryPoint, ChannelName, ChannelGroup } from '../../lib/types'
import { refreshTriggerAtom } from '../atoms'
import type { Setter } from 'jotai'

export interface SeriesSelection {
  keys: string[]
}

// --- extractKeys (moved from PowerHistoryChart) ---

// Case-insensitive: the 1h/6h path produces uppercase keys (ch0_V) from
// reconstructPayload, while the 7d/30d RPC returns lowercase keys (ch0_v)
// directly from Supabase. Both are valid.
const METRIC_REGEX: Record<HistoryMetric, RegExp> = {
  power: /^ch\d_p$/i,
  voltage: /^ch\d_v$/i,
  current: /^ch\d_i$/i,
}
// Pre-compiled alternations for the more specific system/INA keys
const POWER_EXTRA = /^(ina226_p|ina3221_p\d|inverter_power|pv_power|battery_power|dc_load_power)$/i
const VOLTAGE_EXTRA = /^(ina226_v|ina3221_v\d)$/i
const CURRENT_EXTRA = /^(ina226_i|ina3221_i\d|inverter_current)$/i
const EXTRA_REGEX: Record<HistoryMetric, RegExp> = {
  power: POWER_EXTRA,
  voltage: VOLTAGE_EXTRA,
  current: CURRENT_EXTRA,
}

const SYSTEM_POWER_KEYS = ['pv_power', 'battery_power', 'inverter_power', 'dc_load_power']

export function extractKeys(data: TelemetryPoint[], metric: HistoryMetric): string[] {
  if (data.length === 0) return []
  const allKeys = new Set<string>()
  for (const pt of data) {
    Object.keys(pt.payload).forEach(k => allKeys.add(k))
  }
  const regexKeys = Array.from(allKeys).filter(k =>
    METRIC_REGEX[metric].test(k) || EXTRA_REGEX[metric].test(k),
  )
  const nonZero = regexKeys.filter(k => data.some(pt => {
    const v = pt.payload[k]
    return v != null && Math.abs(v) > 0.5
  }))
  if (metric === 'power') {
    const hasSystem = SYSTEM_POWER_KEYS.some(k => nonZero.includes(k))
    if (hasSystem) {
      return nonZero.filter(k => SYSTEM_POWER_KEYS.includes(k) || k === 'ina226_p')
    }
    return nonZero
  }
  if (metric === 'voltage' || metric === 'current') {
    const result: string[] = []
    const seenChannels = new Set<number>()
    for (const k of nonZero) {
      const ch = k.match(/^ch(\d)_[VI]$/i)
      if (ch) { seenChannels.add(parseInt(ch[1])); result.push(k) }
    }
    for (const k of nonZero) {
      const ina = k.match(/^ina3221_[vi](\d)$/)
      if (ina && !seenChannels.has(parseInt(ina[1]))) result.push(k)
    }
    const ina226Key = metric === 'voltage' ? 'ina226_v' : 'ina226_i'
    if (nonZero.includes(ina226Key)) result.push(ina226Key)
    return result
  }
  return nonZero
}

// --- keyToLabel (moved from PowerHistoryChart) ---

function vcName(
  channelNames: ChannelName[] | undefined,
  idx: number,
  groupLabelByChannel?: Map<number, string>,
): string {
  // Priority:
  //   1. Explicit group label from channel_groups (mapped to ch index)
  //   2. Real channel name from device_channels.channel_names
  //   3. Fallback: VC{idx}
  if (groupLabelByChannel?.has(idx)) {
    return groupLabelByChannel.get(idx)!
  }
  const override = channelNames?.find(cn => cn.channel === idx)?.name
  // Treat placeholder-looking names (raw column names) as missing.
  if (override && !/^ch\d+\s*[_ ]?\s*(?:V|P|I|v|p|i)$/i.test(override.trim())) {
    return override
  }
  return `VC${idx}`
}

// Build a map of channel index → friendly label from channel_groups.
// The first channel in a group gets the bare name; subsequent ones get a
// numeric suffix. If a channel belongs to multiple groups, the first one wins.
const GROUP_ICON_LABELS: Record<number, string> = {
  0: 'PV',
  1: 'Battery',
  2: 'DC Load',
  3: 'Load',
}

export function buildChannelLabelMap(channelGroups: ChannelGroup[] | undefined | null): Map<number, string> {
  const map = new Map<number, string>()
  if (!channelGroups) return map
  for (const group of channelGroups) {
    const baseLabel = GROUP_ICON_LABELS[group.icon] ?? `Group ${group.name ?? group.icon}`
    const memberChannels: number[] = []
    for (let ch = 0; ch < 4; ch++) {
      if (group.channel_mask & (1 << ch)) memberChannels.push(ch)
    }
    memberChannels.forEach((ch, i) => {
      if (!map.has(ch)) {
        map.set(ch, memberChannels.length > 1 ? `${baseLabel} ${i + 1}` : baseLabel)
      }
    })
  }
  return map
}

export function keyToLabel(
  k: string,
  channelNames?: ChannelName[],
  groupLabelByChannel?: Map<number, string>,
): string {
  if (k === 'ina226_p') return 'INA226'
  if (k === 'ina226_v') return 'INA226 V'
  if (k === 'ina226_i') return 'INA226 I'
  if (k === 'inverter_current') return 'Inverter I'
  if (k === 'inverter_power') return 'Inverter Output'
  if (k === 'pv_power') return 'PV Generation'
  if (k === 'battery_power') return 'Battery'
  if (k === 'dc_load_power') return 'DC Load'
  if (k === 'soc_pct0') return 'Battery SOC'
  const ina = k.match(/^ina3221_([pvi])(\d)$/)
  if (ina) {
    const m2m: Record<string, string> = { p: 'P', v: 'V', i: 'I' }
    return `${vcName(channelNames, parseInt(ina[2]), groupLabelByChannel)} ${m2m[ina[1]]}`
  }
  const ch = k.match(/^ch(\d)_([pvi])$/)
  if (ch) return `${vcName(channelNames, parseInt(ch[1]), groupLabelByChannel)} ${ch[2].toUpperCase()}`
  return k
}

// --- Range / drilldown ---

export function suggestDrilldown(
  fromRange: HistoryRange,
  _bucketMs: number,
): HistoryRange {
  if (fromRange === '24h') return '1h'
  if (fromRange === '7d') return '1h'
  if (fromRange === '30d') return '6h'
  return '1h'
}

export function bucketToWindow(
  bucketISO: string,
  bucketMs: number,
): { tStart: string; tEnd: string } {
  const t = new Date(bucketISO).getTime()
  return {
    tStart: new Date(t - bucketMs / 2).toISOString(),
    tEnd: new Date(t + bucketMs / 2).toISOString(),
  }
}

export function forceRefresh(set: Setter) {
  set(refreshTriggerAtom, (n: number) => n + 1)
}
