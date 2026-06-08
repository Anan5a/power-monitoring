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
export const SYSTEM_CURRENT_KEYS = ['pv_current', 'battery_current', 'inverter_current', 'dc_load_current']
// Channel-to-system mapping for current, derived the same way the trigger
// derives system power. Currents are always magnitudes (signs live in power),
// so each system current is a sum of |ch_i| across the channels in its group.
export function computeSystemCurrents(
  payload: Record<string, number>,
  channelGroups: ChannelGroup[] | undefined | null,
): Record<string, number> {
  const out: Record<string, number> = {}
  if (!channelGroups || channelGroups.length === 0) return out
  // Accumulate per icon, so multiple groups sharing an icon (e.g. two PV
  // groups, ch0 and ch1) sum into a single pv_current.
  for (const group of channelGroups) {
    let sum = 0
    let any = false
    for (let ch = 0; ch < 4; ch++) {
      if (!(group.channel_mask & (1 << ch))) continue
      const k = `ch${ch}_I`
      // Case-insensitive lookup (RPC returns ch*_i, payload writes ch*_I).
      const v = payload[k] ?? payload[k.toLowerCase()] ?? payload[k.toUpperCase()]
      if (v != null) { sum += Math.abs(v); any = true }
    }
    if (!any) continue
    if (group.icon === 0) out['pv_current'] = (out['pv_current'] ?? 0) + sum
    else if (group.icon === 1) out['battery_current'] = (out['battery_current'] ?? 0) + sum
    else if (group.icon === 2 || group.icon === 3) out['dc_load_current'] = (out['dc_load_current'] ?? 0) + sum
  }
  // Inverter current isn't derivable from per-channel currents alone (it's an
  // energy balance, not a sum of magnitudes). Pass it through from the payload
  // if the device provides it directly.
  const inv = payload['inverter_current'] ?? payload['inverter_I']
  if (inv != null) out['inverter_current'] = inv
  return out
}

// Augment a payload with computed system current keys. Returns the original
// payload unchanged when not relevant (metric !== 'current' or no groups).
export function withSystemCurrents(
  payload: Record<string, number>,
  channelGroups: ChannelGroup[] | undefined | null,
): Record<string, number> {
  const derived = computeSystemCurrents(payload, channelGroups)
  if (Object.keys(derived).length === 0) return payload
  return { ...payload, ...derived }
}

export function extractKeys(
  data: TelemetryPoint[],
  metric: HistoryMetric,
  channelGroups?: ChannelGroup[] | null,
): string[] {
  if (data.length === 0) return []
  const allKeys = new Set<string>()
  for (const pt of data) {
    Object.keys(pt.payload).forEach(k => allKeys.add(k))
  }
  const regexKeys = Array.from(allKeys).filter(k =>
    METRIC_REGEX[metric].test(k) || EXTRA_REGEX[metric].test(k),
  )
  // For the Current metric, augment the payload set with derived system
  // currents (PV/Battery/DC Load) computed from ch*_i + channel_groups, and
  // include inverter_current if present. Mirrors the Power tab behaviour.
  if (metric === 'current' && channelGroups && channelGroups.length > 0) {
    for (const pt of data) {
      const derived = computeSystemCurrents(pt.payload, channelGroups)
      for (const k of Object.keys(derived)) {
        // Use a lower threshold (0.1 A) than the global 0.5 — small but
        // meaningful loads/battery currents shouldn't be hidden.
        if (derived[k] != null && Math.abs(derived[k]) > 0.1) {
          regexKeys.push(k)
        }
      }
    }
  }
  const nonZero = regexKeys.filter(k => {
    // System current keys (derived) bypass the 0.5 filter — they're already
    // thresholded above at 0.1 and are aggregate signals worth showing.
    const isSystemCurrent = SYSTEM_CURRENT_KEYS.includes(k)
    const threshold = isSystemCurrent ? 0.05 : 0.5
    return data.some(pt => {
      const raw = pt.payload[k]
      const v = raw != null
        ? raw
        : (metric === 'current' ? computeSystemCurrents(pt.payload, channelGroups)[k] : undefined)
      return v != null && Math.abs(v) > threshold
    })
  })
  if (metric === 'power') {
    const hasSystem = SYSTEM_POWER_KEYS.some(k => nonZero.includes(k))
    if (hasSystem) {
      return nonZero.filter(k => SYSTEM_POWER_KEYS.includes(k) || k === 'ina226_p')
    }
    return nonZero
  }
  if (metric === 'voltage' || metric === 'current') {
    const result: string[] = []
    // For current: prepend system current keys in a fixed display order
    // (PV → Battery → Inverter → DC Load), so the legend stays readable.
    if (metric === 'current') {
      for (const k of SYSTEM_CURRENT_KEYS) {
        if (nonZero.includes(k) && !result.includes(k)) result.push(k)
      }
    }
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

// Map of channel index → friendly label from channel_groups.
// Priority: explicit `name` from the group (e.g. "PV Voltage") wins, then the
// icon-derived base label. When two groups share an icon (e.g. icon=0 = PV
// split across ch0 and ch1), their distinct names keep the legend readable;
// if a group has no name we fall back to "PV", "PV 1", "PV 2" numbering.
const GROUP_ICON_LABELS: Record<number, string> = {
  0: 'PV',
  1: 'Battery',
  2: 'DC Load',
  3: 'Load',
}

export function buildChannelLabelMap(channelGroups: ChannelGroup[] | undefined | null): Map<number, string> {
  const map = new Map<number, string>()
  if (!channelGroups) return map

  // First pass: count how many groups share each icon, so we can disambiguate
  // unnamed groups with a numeric suffix ("PV 1", "PV 2").
  const iconCounts: Record<number, number> = {}
  for (const g of channelGroups) {
    iconCounts[g.icon] = (iconCounts[g.icon] ?? 0) + 1
  }
  const iconSeen: Record<number, number> = {}

  for (const group of channelGroups) {
    const baseLabel = GROUP_ICON_LABELS[group.icon] ?? `Group ${group.name ?? group.icon}`
    // Prefer the user-supplied name when it looks like a real label.
    const explicitName = (group.name ?? '').trim()
    const isPlaceholder = /^ch\d+\s*[_ ]?\s*(?:V|P|I|v|p|i)$/i.test(explicitName) || explicitName === ''
    const memberChannels: number[] = []
    for (let ch = 0; ch < 4; ch++) {
      if (group.channel_mask & (1 << ch)) memberChannels.push(ch)
    }
    // Numbering within a single group: "PV 1", "PV 2" when one group has >1 ch.
    // Across multiple groups sharing an icon, number unnamed ones so they
    // don't collide: "PV", "PV 2", "PV 3"...
    memberChannels.forEach((ch, i) => {
      if (map.has(ch)) return
      let label: string
      if (!isPlaceholder) {
        label = memberChannels.length > 1 ? `${explicitName} ${i + 1}` : explicitName
      } else {
        iconSeen[group.icon] = (iconSeen[group.icon] ?? 0) + 1
        const idx = iconSeen[group.icon]
        label = iconCounts[group.icon] > 1 ? `${baseLabel} ${idx}` : baseLabel
      }
      map.set(ch, label)
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
  if (k === 'pv_current') return 'PV Current'
  if (k === 'battery_power') return 'Battery'
  if (k === 'battery_current') return 'Battery Current'
  if (k === 'dc_load_power') return 'DC Load'
  if (k === 'dc_load_current') return 'DC Load Current'
  if (k === 'soc_pct0') return 'Battery SOC'
  const ina = k.match(/^ina3221_([pvi])(\d)$/)
  if (ina) {
    const m2m: Record<string, string> = { p: 'P', v: 'V', i: 'I' }
    return `${vcName(channelNames, parseInt(ina[2]), groupLabelByChannel)} ${m2m[ina[1]]}`
  }
  const ch = k.match(/^ch(\d)_([pvi])$/i)
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
