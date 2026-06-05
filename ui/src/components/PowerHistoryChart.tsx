import { useState, useEffect, useMemo } from 'react'
import { motion, AnimatePresence } from 'framer-motion'
import {
  AreaChart, Area, XAxis, YAxis, CartesianGrid, Tooltip,
  ResponsiveContainer, Scatter, Line, ReferenceLine
} from 'recharts'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint, DeviceChannels, ChannelName } from '../lib/types'

interface Props {
  deviceKey: string
  deviceChannels?: DeviceChannels | null
}

type Range = '1h' | '6h' | '24h' | '7d' | '30d'
type Metric = 'power' | 'voltage' | 'current'

const RANGE_HOURS: Record<Range, number> = { '1h': 1, '6h': 6, '24h': 24, '7d': 168, '30d': 720 }
const RANGE_LIMITS: Record<Range, number> = { '1h': 1000, '6h': 5000, '24h': 20000, '7d': 50000, '30d': 50000 }
// Downsample to ~1500 points max for smooth chart rendering
const DOWNSAMPLE_TARGET = 1500

const UNIT: Record<Metric, string> = { power: 'W', voltage: 'V', current: 'A' }

// Vibrant gradient palette — each line gets a unique gradient
const SERIES_GRADIENTS: Record<string, { id: string; start: string; end: string }> = {
  // Power
  ina3221_p0: { id: 'grad_p0', start: '#3b82f6', end: '#93c5fd' },
  ina3221_p1: { id: 'grad_p1', start: '#22c55e', end: '#86efac' },
  ina3221_p2: { id: 'grad_p2', start: '#f59e0b', end: '#fcd34d' },
  ina226_p:   { id: 'grad_ina', start: '#a855f7', end: '#d8b4fe' },
  ch0_P:      { id: 'grad_ch0', start: '#06b6d4', end: '#67e8f9' },
  ch1_P:      { id: 'grad_ch1', start: '#ec4899', end: '#f9a8d4' },
  ch2_P:      { id: 'grad_ch2', start: '#f97316', end: '#fdba74' },
  ch3_P:      { id: 'grad_ch3', start: '#84cc16', end: '#bef264' },
  pv_power:        { id: 'grad_pv', start: '#f59e0b', end: '#fcd34d' },
  battery_power:   { id: 'grad_bat', start: '#10b981', end: '#6ee7b7' },
  inverter_power:  { id: 'grad_inv', start: '#3b82f6', end: '#93c5fd' },
  dc_load_power:   { id: 'grad_dc', start: '#ef4444', end: '#fca5a5' },
  soc_pct0:        { id: 'grad_soc', start: '#a855f7', end: '#d8b4fe' },
  // Voltage
  ina3221_v0: { id: 'grad_v0', start: '#0ea5e9', end: '#7dd3fc' },
  ina3221_v1: { id: 'grad_v1', start: '#10b981', end: '#6ee7b7' },
  ina3221_v2: { id: 'grad_v2', start: '#f59e0b', end: '#fcd34d' },
  ina226_v:   { id: 'grad_inav', start: '#8b5cf6', end: '#c4b5fd' },
  ch0_V:      { id: 'grad_ch0v', start: '#0891b2', end: '#67e8f9' },
  ch1_V:      { id: 'grad_ch1v', start: '#db2777', end: '#f9a8d4' },
  ch2_V:      { id: 'grad_ch2v', start: '#ea580c', end: '#fdba74' },
  ch3_V:      { id: 'grad_ch3v', start: '#65a30d', end: '#bef264' },
  // Current
  ina3221_i0: { id: 'grad_i0', start: '#2563eb', end: '#93c5fd' },
  ina3221_i1: { id: 'grad_i1', start: '#16a34a', end: '#86efac' },
  ina3221_i2: { id: 'grad_i2', start: '#d97706', end: '#fcd34d' },
  ina226_i:   { id: 'grad_inai', start: '#9333ea', end: '#d8b4fe' },
  ch0_I:      { id: 'grad_ch0i', start: '#0284c7', end: '#7dd3fc' },
  ch1_I:      { id: 'grad_ch1i', start: '#be185d', end: '#f9a8d4' },
  ch2_I:      { id: 'grad_ch2i', start: '#c2410c', end: '#fdba74' },
  ch3_I:      { id: 'grad_ch3i', start: '#4d7c0f', end: '#bef264' },
  inverter_current: { id: 'grad_invi', start: '#0ea5e9', end: '#7dd3fc' },
}

// Fallback palette for unregistered keys
const FALLBACK_COLORS = [
  { start: '#6366f1', end: '#a5b4fc' },
  { start: '#14b8a6', end: '#5eead4' },
  { start: '#f43f5e', end: '#fda4af' },
  { start: '#8b5cf6', end: '#c4b5fd' },
  { start: '#0ea5e9', end: '#7dd3fc' },
  { start: '#10b981', end: '#6ee7b7' },
]

const METRIC_REGEX: Record<Metric, RegExp> = {
  power: /^ch\d_P$|ina226_p|^ina3221_p\d$|inverter_power|pv_power|battery_power|dc_load_power/,
  voltage: /^ch\d_V$|ina226_v|^ina3221_v\d$/,
  current: /^ch\d_I$|ina226_i|^ina3221_i\d$|inverter_current$/,
}

// For power tab, prefer computed system columns over raw channel powers
const SYSTEM_POWER_KEYS = ['pv_power', 'battery_power', 'inverter_power', 'dc_load_power']

function extractKeys(data: TelemetryPoint[], metric: Metric): string[] {
  if (data.length === 0) return []
  // Collect all unique keys across ALL data points (not just first)
  const allKeys = new Set<string>()
  for (const pt of data) {
    Object.keys(pt.payload as Record<string, number>).forEach(k => allKeys.add(k))
  }
  const regexKeys = Array.from(allKeys).filter(k => METRIC_REGEX[metric].test(k))

  // Filter out keys where all values are 0 or null
  const nonZeroKeys = regexKeys.filter(k => {
    return data.some(pt => {
      const v = (pt.payload as Record<string, number>)[k]
      return v != null && Math.abs(v) > 0.5
    })
  })

  // For power tab: if computed system columns exist, exclude raw ch*_P that overlap
  if (metric === 'power') {
    const hasSystemKeys = SYSTEM_POWER_KEYS.some(k => nonZeroKeys.includes(k))
    if (hasSystemKeys) {
      return nonZeroKeys.filter(k => SYSTEM_POWER_KEYS.includes(k) || k === 'ina226_p')
    }
  }

  // For voltage/current: if both virtual channel (chN_V/I) and raw INA3221 (ina3221_vN/iN)
  // exist for the same channel, prefer the virtual channel to avoid duplicate legend entries.
  if (metric === 'voltage' || metric === 'current') {
    const result: string[] = []
    const seenChannels = new Set<number>()
    for (const k of nonZeroKeys) {
      const ch = k.match(/^ch(\d)_[VI]$/i)
      if (ch) {
        seenChannels.add(parseInt(ch[1]))
        result.push(k)
      }
    }
    for (const k of nonZeroKeys) {
      const ina = k.match(/^ina3221_[vi](\d)$/)
      if (ina) {
        if (!seenChannels.has(parseInt(ina[1]))) {
          result.push(k)
        }
      }
    }
    const ina226Key = metric === 'voltage' ? 'ina226_v' : 'ina226_i'
    if (nonZeroKeys.includes(ina226Key)) result.push(ina226Key)
    return result
  }

  return nonZeroKeys
}

function vcName(channelNames: ChannelName[] | undefined, idx: number): string {
  return channelNames?.find(cn => cn.channel === idx)?.name ?? `VC${idx}`
}

function keyToLabel(k: string, channelNames?: ChannelName[]): string {
  if (k === 'ina226_p') return 'INA226'
  if (k === 'ina226_v') return 'INA226 V'
  if (k === 'ina226_i') return 'INA226 I'
  if (k === 'inverter_current') return 'Inverter I'
  if (k === 'inverter_power') return 'Inverter Output'
  if (k === 'pv_power') return 'PV Generation'
  if (k === 'battery_power') return 'Battery'
  if (k === 'dc_load_power') return 'DC Load'
  if (k === 'soc_pct0') return 'Battery SOC'
  const ina = k.match(/^ina3221_([pvi])([0-2])$/)
  if (ina) {
    const m2m: Record<string, string> = { p: 'P', v: 'V', i: 'I' }
    return `${vcName(channelNames, parseInt(ina[2]))} ${m2m[ina[1]]}`
  }
  const ch = k.match(/^ch(\d)_([PVI])$/i)
  if (ch) return `${vcName(channelNames, parseInt(ch[1]))} ${ch[2].toUpperCase()}`
  return k
}

export default function PowerHistoryChart({ deviceKey, deviceChannels }: Props) {
  const channelNames = deviceChannels?.channel_names
  const [range, setRange] = useState<Range>('24h')
  const [metric, setMetric] = useState<Metric>('power')
  const [historyData, setHistoryData] = useState<TelemetryPoint[]>([])
  const [seriesKeys, setSeriesKeys] = useState<string[]>([])
  const [loading, setLoading] = useState(false)
  const [visibleLines, setVisibleLines] = useState<Set<string>>(new Set())
  const [pollTick, setPollTick] = useState(0)

  useEffect(() => {
    const id = setInterval(() => setPollTick(t => t + 1), 15000)
    return () => clearInterval(id)
  }, [])

  useEffect(() => {
    if (!deviceKey) return
    setLoading(true)
    const hours = RANGE_HOURS[range]

    // Short ranges: query telemetry_computed typed columns
    if (range === '1h' || range === '6h') {
      const since = new Date(Date.now() - hours * 3600 * 1000).toISOString()
      supabase
        .from('telemetry_computed')
        .select('*')
        .eq('device_key', deviceKey)
        .gte('recorded_at', since)
        .order('recorded_at', { ascending: true })
        .limit(RANGE_LIMITS[range])
        .then(({ data }) => {
          if (data) {
            // Reconstruct payload from typed columns
            const typed = data as Array<Record<string, unknown>>
            const points = typed.map(row => ({
              id: (row.id as number) ?? 0,
              device_id: row.device_key as string,
              recorded_at: row.recorded_at as string,
              payload: {
                ...(row.ch0_v != null && { 'ch0_V': row.ch0_v }),
                ...(row.ch0_i != null && { 'ch0_I': row.ch0_i }),
                ...(row.ch0_p != null && { 'ch0_P': row.ch0_p }),
                ...(row.ch1_v != null && { 'ch1_V': row.ch1_v }),
                ...(row.ch1_i != null && { 'ch1_I': row.ch1_i }),
                ...(row.ch1_p != null && { 'ch1_P': row.ch1_p }),
                ...(row.ch2_v != null && { 'ch2_V': row.ch2_v }),
                ...(row.ch2_i != null && { 'ch2_I': row.ch2_i }),
                ...(row.ch2_p != null && { 'ch2_P': row.ch2_p }),
                ...(row.ch3_v != null && { 'ch3_V': row.ch3_v }),
                ...(row.ch3_i != null && { 'ch3_I': row.ch3_i }),
                ...(row.ch3_p != null && { 'ch3_P': row.ch3_p }),
                ...(row.ina3221_v0 != null && { 'ina3221_v0': row.ina3221_v0 }),
                ...(row.ina3221_v1 != null && { 'ina3221_v1': row.ina3221_v1 }),
                ...(row.ina3221_v2 != null && { 'ina3221_v2': row.ina3221_v2 }),
                ...(row.ina3221_i0 != null && { 'ina3221_i0': row.ina3221_i0 }),
                ...(row.ina3221_i1 != null && { 'ina3221_i1': row.ina3221_i1 }),
                ...(row.ina3221_i2 != null && { 'ina3221_i2': row.ina3221_i2 }),
                ...(row.ina226_v != null && { 'ina226_v': row.ina226_v }),
                ...(row.ina226_i != null && { 'ina226_i': row.ina226_i }),
                ...(row.ina226_p != null && { 'ina226_p': row.ina226_p }),
                ...(row.pv_power != null && { 'pv_power': row.pv_power }),
                ...(row.battery_power != null && { 'battery_power': row.battery_power }),
                ...(row.inverter_power != null && { 'inverter_power': row.inverter_power }),
                ...(row.inverter_current != null && { 'inverter_current': row.inverter_current }),
                ...(row.dc_load_power != null && { 'dc_load_power': row.dc_load_power }),
                ...(row.ads1115_0 != null && { 'ads1115_0': row.ads1115_0 }),
                ...(row.ads1115_1 != null && { 'ads1115_1': row.ads1115_1 }),
                ...(row.ads1115_2 != null && { 'ads1115_2': row.ads1115_2 }),
                ...(row.ads1115_3 != null && { 'ads1115_3': row.ads1115_3 }),
                ...(row.energy_wh0 != null && { 'energy_wh0': row.energy_wh0 }),
                ...(row.energy_wh1 != null && { 'energy_wh1': row.energy_wh1 }),
                ...(row.energy_wh2 != null && { 'energy_wh2': row.energy_wh2 }),
                ...(row.energy_wh3 != null && { 'energy_wh3': row.energy_wh3 }),
                ...(row.soc_pct0 != null && { 'soc_pct0': row.soc_pct0 }),
                ...(row.soc_pct1 != null && { 'soc_pct1': row.soc_pct1 }),
                ...(row.soc_pct2 != null && { 'soc_pct2': row.soc_pct2 }),
                ...(row.soc_pct3 != null && { 'soc_pct3': row.soc_pct3 }),
                ...(row.ina3221_v0_spike === true && { 'ina3221_v0_spike': true }),
                ...(row.ina3221_v1_spike === true && { 'ina3221_v1_spike': true }),
                ...(row.ina3221_v2_spike === true && { 'ina3221_v2_spike': true }),
                ...(row.ina3221_i0_spike === true && { 'ina3221_i0_spike': true }),
                ...(row.ina3221_i1_spike === true && { 'ina3221_i1_spike': true }),
                ...(row.ina3221_i2_spike === true && { 'ina3221_i2_spike': true }),
              },
              metadata: {},
            }))
            setHistoryData(points as TelemetryPoint[])
            const keys = extractKeys(points as TelemetryPoint[], metric)
            setSeriesKeys(keys)
            setVisibleLines(new Set(keys))
          }
          setLoading(false)
        })
      return
    }

    // Long ranges: use RPC aggregation
    supabase
      .rpc('get_aggregated_telemetry', {
        p_device_key: deviceKey,
        p_hours: hours,
        p_metric: metric,
      })
      .then(({ data, error }) => {
        if (error) {
          console.error('RPC error:', error)
          setLoading(false)
          return
        }
        if (data && data.length > 0) {
          // Transform compact RPC rows into TelemetryPoint shape
          const arr: TelemetryPoint[] = (data as Array<Record<string, unknown>>).map((row) => {
            const payload: Record<string, number> = {}
            for (const [key, val] of Object.entries(row)) {
              if (key === 'bucket') continue
              if (val != null && typeof val === 'number') {
                payload[key] = val
              }
            }
            return {
              id: 0,
              device_id: deviceKey,
              recorded_at: row.bucket as string,
              payload,
              metadata: {},
            }
          })
          setHistoryData(arr)
          const keys = extractKeys(arr, metric)
          setSeriesKeys(keys)
          setVisibleLines(new Set(keys))
        }
        setLoading(false)
      })
  }, [deviceKey, range, metric, pollTick])

  // Re-extract visible keys when metric changes (so empty filter from old metric doesn't persist)
  useEffect(() => {
    if (historyData.length === 0) return
    const keys = extractKeys(historyData, metric)
    setSeriesKeys(keys)
    setVisibleLines(new Set(keys))
  }, [metric, historyData])

  // Downsample for smooth rendering on long ranges
  const downsampledHistory = useMemo(() => {
    if (historyData.length <= DOWNSAMPLE_TARGET) return historyData
    const step = Math.ceil(historyData.length / DOWNSAMPLE_TARGET)
    return historyData.filter((_, i) => i % step === 0)
  }, [historyData])

  const chartData = useMemo(() => downsampledHistory.map(pt => {
    const d = new Date(pt.recorded_at)
    // For 24h view, show just time unless crossing midnight; for longer ranges show date+time
    const time = range === '24h'
      ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
      : range === '1h' || range === '6h'
        ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
        : d.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    const rec: Record<string, unknown> = { time }
    for (const k of seriesKeys) {
      let val = (pt.payload as Record<string, number>)[k] ?? null
      rec[k] = val
    }
    // Pull SOC for power view (rendered as secondary axis line)
    if (metric === 'power') {
      const soc0 = (pt.payload as Record<string, number>)['soc_pct0']
      if (soc0 != null) rec['soc_pct0'] = soc0
    }
    return rec
  }), [downsampledHistory, seriesKeys, range, metric])

  // Spike markers (only meaningful on power tab)
  const spikeData: Array<{ time: string; spike_y: number }> = useMemo(() => {
    if (metric !== 'power') return []
    const out: Array<{ time: string; spike_y: number }> = []
    for (const pt of downsampledHistory) {
      const payload = pt.payload as Record<string, unknown>
      for (let i = 0; i < 3; i++) {
        if (payload[`ina3221_i${i}_spike`] === true) {
          const d = new Date(pt.recorded_at)
          const time = range === '24h'
            ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
            : range === '1h' || range === '6h'
              ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
              : d.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
          out.push({ time, spike_y: 0 })
          break
        }
      }
    }
    return out
  }, [downsampledHistory, metric, range])

  const toggleLine = (key: string) => {
    const next = new Set(visibleLines)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    setVisibleLines(next)
  }

  const toggleAll = () => {
    if (visibleLines.size === seriesKeys.length) {
      setVisibleLines(new Set())
    } else {
      setVisibleLines(new Set(seriesKeys))
    }
  }

  const visibleKeys = seriesKeys.filter(k => visibleLines.has(k))


  // Solis-style: stack power areas in fixed order so they layer predictably
  const chartKeys = metric === 'power'
    ? [...visibleKeys].sort((a, b) => {
        const order = ['inverter_power', 'dc_load_power', 'battery_power', 'pv_power', 'ina226_p']
        return order.indexOf(a) - order.indexOf(b)
      })
    : visibleKeys

  const metricLabel = { power: 'Power', voltage: 'Voltage', current: 'Current' }[metric]
  const unit = UNIT[metric]
  const showDualAxis = metric !== 'power'

  // Custom tooltip with all visible metrics
  interface TooltipPayload {
    payload?: Record<string, unknown>
    name?: string
    value?: number
    dataKey?: string
    color?: string
  }
  const CustomTooltip = ({ active, payload, label }: { active?: boolean; payload?: TooltipPayload[]; label?: string }) => {
    if (!active || !payload || payload.length === 0) return null
    // Recharts sometimes omits Line-series from payload when mixed with Area.
    // Render directly from the hovered data point so all visible keys appear.
    const hoveredPoint = payload[0]?.payload as Record<string, unknown> | undefined
    if (!hoveredPoint) return null
    return (
      <div className="bg-slate-800 rounded-xl shadow-lg px-3 py-2.5 min-w-[140px]">
        <div className="text-[11px] text-slate-400 mb-1.5 font-medium">{label}</div>
        {visibleKeys.map((k, i) => {
          const v = hoveredPoint[k] as number | undefined
          if (typeof v !== 'number') return null
          const g = SERIES_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]
          return (
            <div key={k} className="flex items-center justify-between gap-3 text-[12px] py-0.5">
              <div className="flex items-center gap-1.5">
                <span className="w-2 h-2 rounded-full flex-shrink-0" style={{ background: g.start }} />
                <span className="text-slate-300">{keyToLabel(k, channelNames)}</span>
              </div>
              <span className="text-slate-100 font-semibold font-mono">
                {k === 'soc_pct0'
                  ? `${v.toFixed(0)} %`
                  : `${Math.abs(v).toFixed(metric === 'voltage' ? 2 : 1)} ${unit}`}
              </span>
            </div>
          )
        })}
      </div>
    )
  }

  return (
    <div className="bg-white rounded-2xl shadow-[0_1px_2px_rgba(15,23,42,0.04),0_1px_3px_rgba(15,23,42,0.06)] border border-slate-100 overflow-hidden">
      {/* Header */}
      <div className="flex items-center justify-between px-5 py-4 border-b border-slate-100">
        <div className="flex items-center gap-2">
          <h3 className="font-bold text-slate-800 text-base">History</h3>
          {loading && (
            <span className="flex items-center gap-1 text-xs text-slate-400">
              <span className="w-1.5 h-1.5 rounded-full bg-cyan-400 animate-pulse" />
              loading
            </span>
          )}
        </div>
        <div className="flex items-center gap-1">
          {(['1h', '6h', '24h', '7d', '30d'] as Range[]).map(r => (
            <button
              key={r}
              onClick={() => setRange(r)}
              className={`px-3 py-1 rounded-lg text-xs font-semibold transition-all duration-150 ${
                range === r
                  ? 'bg-gradient-to-r from-cyan-500 to-blue-500 text-white shadow-sm'
                  : 'bg-slate-100 text-slate-500 hover:bg-slate-200'
              }`}
            >
              {r}
            </button>
          ))}
        </div>
      </div>

      {/* Metric tabs */}
      <div className="flex items-center gap-1.5 px-5 pt-4">
        {(['power', 'voltage', 'current'] as Metric[]).map(m => (
          <button
            key={m}
            onClick={() => setMetric(m)}
            className={`px-4 py-1.5 rounded-full text-xs font-semibold transition-all duration-150 ${
              metric === m
                ? 'bg-gradient-to-r from-cyan-500 to-blue-500 text-white shadow-sm'
                : 'bg-slate-100 text-slate-500 hover:bg-slate-200'
            }`}
          >
            {m.charAt(0).toUpperCase() + m.slice(1)}
          </button>
        ))}
      </div>

      {/* Legend */}
      {seriesKeys.length > 0 && (
        <div className="flex items-center gap-2 px-5 pt-3 pb-2 flex-wrap">
          <button
            onClick={toggleAll}
            className="text-xs px-2 py-0.5 rounded border border-slate-200 text-slate-500 hover:bg-slate-50 transition-colors"
          >
            {visibleLines.size === seriesKeys.length ? 'Hide all' : 'Show all'}
          </button>
          <div className="w-px h-4 bg-slate-200" />
          {seriesKeys.map((k, i) => {
            const g = SERIES_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]
            const active = visibleLines.has(k)
            return (
              <button
                key={k}
                onClick={() => toggleLine(k)}
                className={`flex items-center gap-1.5 text-xs px-2 py-0.5 rounded-full border transition-all duration-150 ${
                  active
                    ? 'border-transparent shadow-sm'
                    : 'border-slate-200 text-slate-400 opacity-60'
                }`}
                style={active ? { backgroundColor: g.start + '18', borderColor: g.start + '40' } : {}}
              >
                <span
                  className="w-2.5 h-2.5 rounded-full flex-shrink-0"
                  style={{ background: `linear-gradient(135deg, ${g.start}, ${g.end})` }}
                />
                <span style={active ? { color: g.start } : {}}>{keyToLabel(k, channelNames)}</span>
              </button>
            )
          })}
        </div>
      )}

      {/* Chart */}
      <div className="px-2 pb-4">
        <AnimatePresence mode="wait">
          <motion.div
            key={`${metric}-${range}`}
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.2 }}
          >
            {loading && chartData.length === 0 ? (
              <div className="h-72 flex flex-col items-center justify-center text-slate-300 gap-3">
                <svg className="w-10 h-10 animate-spin" viewBox="0 0 24 24" fill="none">
                  <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
                  <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
                </svg>
                <span className="text-sm">Fetching {metricLabel.toLowerCase()} data…</span>
              </div>
            ) : chartData.length === 0 ? (
              <div className="h-72 flex flex-col items-center justify-center text-slate-400 gap-2">
                <svg viewBox="0 0 24 24" className="w-10 h-10 text-slate-300" fill="none" stroke="currentColor" strokeWidth="1.5">
                  <path strokeLinecap="round" strokeLinejoin="round" d="M7 12.5l4-4m0 0l4 4m-4-4v12m8-12a9 9 0 11-18 0 9 9 0 0118 0z" />
                </svg>
                <span className="text-sm font-medium">No {metricLabel.toLowerCase()} data for this range</span>
                <span className="text-xs text-slate-300">Data arrives every 5 seconds</span>
              </div>
            ) : (
              <ResponsiveContainer width="100%" height={300}>
                <AreaChart data={chartData} margin={{ top: 10, right: 16, left: -8, bottom: 0 }}>
                  <CartesianGrid
                    strokeDasharray="0"
                    stroke="#f1f5f9"
                    strokeWidth={1}
                    vertical={false}
                  />
                  <XAxis
                    dataKey="time"
                    tick={{ fontSize: 11, fill: '#94a3b8', fontFamily: 'inherit' }}
                    axisLine={false}
                    tickLine={false}
                    interval={chartData.length > 20 ? Math.ceil(chartData.length / 10) : 'preserveStartEnd'}
                  />
                  <YAxis
                    yAxisId="left"
                    tick={{ fontSize: 11, fill: '#94a3b8', fontFamily: 'inherit' }}
                    axisLine={false}
                    tickLine={false}
                    unit={` ${unit}`}
                    width={56}
                  />
                  {showDualAxis && (
                    <YAxis
                      yAxisId="right"
                      orientation="right"
                      tick={{ fontSize: 11, fill: '#94a3b8', fontFamily: 'inherit' }}
                      axisLine={false}
                      tickLine={false}
                      unit={` ${unit}`}
                      width={56}
                    />
                  )}
                  {metric === 'power' && chartData.some(d => d.soc_pct0 != null) && (
                    <YAxis
                      yAxisId="soc"
                      orientation="right"
                      tick={{ fontSize: 11, fill: '#a855f7', fontFamily: 'inherit' }}
                      axisLine={false}
                      tickLine={false}
                      unit=" %"
                      domain={[0, 100]}
                      width={48}
                    />
                  )}
                  <Tooltip
                    content={<CustomTooltip />}
                    cursor={{ stroke: '#94a3b8', strokeWidth: 1, strokeDasharray: '3 3' }}
                  />
                  {metric === 'power' && (
                    <ReferenceLine
                      yAxisId="left"
                      y={0}
                      stroke="#cbd5e1"
                      strokeWidth={1}
                      strokeDasharray="4 4"
                    />
                  )}
                  {chartKeys.map((k, i) => {
                    const g = SERIES_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]

                    // PV Generation: render as a normal shaded Area (same styling as others)
                    if (k === 'pv_power') {
                      return (
                        <Area
                          key={k}
                          type="monotone"
                          dataKey={k}
                          yAxisId={showDualAxis ? 'right' : 'left'}
                          stroke={g.start}
                          strokeWidth={2.5}
                          fill={g.start}
                          fillOpacity={0.25}
                          dot={false}
                          activeDot={{
                            r: 6,
                            fill: g.start,
                            stroke: '#fff',
                            strokeWidth: 2,
                          }}
                          connectNulls
                        />
                      )
                    }

                    return (
                      <Area
                        key={k}
                        type="monotone"
                        dataKey={k}
                        yAxisId={showDualAxis ? 'right' : 'left'}
                        stroke={g.start}
                        strokeWidth={2.5}
                        fill={g.start}
                        fillOpacity={0.25}
                        dot={false}
                        activeDot={{
                          r: 5,
                          fill: g.start,
                          stroke: '#fff',
                          strokeWidth: 2,
                        }}
                        connectNulls
                      />
                    )
                  })}
                  {metric === 'power' && chartData.some(d => d.soc_pct0 != null) && (
                    <Line
                      yAxisId="soc"
                      type="monotone"
                      dataKey="soc_pct0"
                      stroke="#a855f7"
                      strokeWidth={2}
                      dot={false}
                      connectNulls
                      activeDot={{
                        r: 4,
                        fill: '#a855f7',
                        stroke: '#fff',
                        strokeWidth: 2,
                      }}
                    />
                  )}
                  {spikeData.length > 0 && (
                    <Scatter
                      yAxisId={showDualAxis ? 'right' : 'left'}
                      data={spikeData}
                      dataKey="spike_y"
                      line={false}
                      fill="#ef4444"
                      shape="triangle"
                    />
                  )}
                </AreaChart>
              </ResponsiveContainer>
            )}
          </motion.div>
        </AnimatePresence>
      </div>
    </div>
  )
}
