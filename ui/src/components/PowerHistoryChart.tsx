import { useState, useEffect } from 'react'
import {
  AreaChart, Area, XAxis, YAxis, CartesianGrid, Tooltip,
  ResponsiveContainer, Scatter
} from 'recharts'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

interface Props {
  deviceKey: string
}

type Range = '1h' | '6h' | '24h' | '7d' | '30d'

const RANGE_HOURS: Record<Range, number> = { '1h': 1, '6h': 6, '24h': 24, '7d': 168, '30d': 720 }
const RANGE_LIMITS: Record<Range, number> = { '1h': 500, '6h': 500, '24h': 200, '7d': 500, '30d': 500 }

// Vibrant gradient palette — each line gets a unique gradient
const VC_GRADIENTS: Record<string, { id: string; start: string; end: string }> = {
  ina3221_p0: { id: 'grad_p0', start: '#3b82f6', end: '#93c5fd' },
  ina3221_p1: { id: 'grad_p1', start: '#22c55e', end: '#86efac' },
  ina3221_p2: { id: 'grad_p2', start: '#f59e0b', end: '#fcd34d' },
  ina226_p:   { id: 'grad_ina', start: '#a855f7', end: '#d8b4fe' },
  ch0_P:      { id: 'grad_ch0', start: '#06b6d4', end: '#67e8f9' },
  ch1_P:      { id: 'grad_ch1', start: '#ec4899', end: '#f9a8d4' },
  ch2_P:      { id: 'grad_ch2', start: '#f97316', end: '#fdba74' },
  ch3_P:      { id: 'grad_ch3', start: '#84cc16', end: '#bef264' },
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

function extractVCPowerKeys(data: TelemetryPoint[]): string[] {
  if (data.length === 0) return []
  const payloadKeys = Object.keys(data[0].payload as Record<string, number>)
  return payloadKeys.filter(k => k.match(/^ch\d_P$/) || k === 'ina226_p' || k.match(/^ina3221_p\d$/))
}

function keyToLabel(k: string): string {
  if (k === 'ina226_p') return 'INA226'
  if (k === 'ina3221_p0') return 'VC0'
  if (k === 'ina3221_p1') return 'VC1'
  if (k === 'ina3221_p2') return 'VC2'
  return k.replace(/^ch(\d)_P$/i, 'VC$1').toUpperCase()
}

export default function PowerHistoryChart({ deviceKey }: Props) {
  const [range, setRange] = useState<Range>('24h')
  const [historyData, setHistoryData] = useState<TelemetryPoint[]>([])
  const [powerKeys, setPowerKeys] = useState<string[]>([])
  const [loading, setLoading] = useState(false)
  const [visibleLines, setVisibleLines] = useState<Set<string>>(new Set())

  useEffect(() => {
    if (!deviceKey) return
    setLoading(true)
    const hours = RANGE_HOURS[range]
    const since = new Date(Date.now() - hours * 3600 * 1000).toISOString()
    supabase
      .from('telemetry_live')
      .select('*')
      .eq('device_id', deviceKey)
      .gte('recorded_at', since)
      .order('recorded_at', { ascending: true })
      .limit(RANGE_LIMITS[range])
      .then(({ data }) => {
        if (data) {
          setHistoryData(data as TelemetryPoint[])
          const keys = extractVCPowerKeys(data as TelemetryPoint[])
          setPowerKeys(keys)
          setVisibleLines(new Set(keys))
        }
        setLoading(false)
      })
  }, [deviceKey, range])

  const chartData = historyData.map(pt => {
    const rec: Record<string, unknown> = {
      time: new Date(pt.recorded_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }),
    }
    for (const k of powerKeys) {
      rec[k] = (pt.payload as Record<string, number>)[k] ?? null
    }
    return rec
  })

  // Spike markers
  const spikeData: Array<{ time: string; spike_y: number }> = []
  for (const pt of historyData) {
    const payload = pt.payload as Record<string, unknown>
    for (let i = 0; i < 3; i++) {
      if (payload[`ina3221_i${i}_spike`] === true) {
        spikeData.push({
          time: new Date(pt.recorded_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }),
          spike_y: 0,
        })
        break
      }
    }
  }

  const toggleLine = (key: string) => {
    const next = new Set(visibleLines)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    setVisibleLines(next)
  }

  const toggleAll = () => {
    if (visibleLines.size === powerKeys.length) {
      setVisibleLines(new Set())
    } else {
      setVisibleLines(new Set(powerKeys))
    }
  }

  const visibleKeys = powerKeys.filter(k => visibleLines.has(k))

  // Build unique gradient defs for visible lines
  const gradientDefs = visibleKeys.map((k, i) => {
    const g = VC_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]
    return (
      <defs key={g.id + k}>
        <linearGradient id={g.id + k} x1="0" y1="0" x2="0" y2="1">
          <stop offset="5%" stopColor={g.start} stopOpacity={0.4} />
          <stop offset="95%" stopColor={g.end} stopOpacity={0.05} />
        </linearGradient>
      </defs>
    )
  })

  // Build gradient fill defs for each area
  const areaGradients = visibleKeys.map((k, i) => {
    const g = VC_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]
    const gradId = `area_fill_${k}`
    return (
      <defs key={`area_${k}`}>
        <linearGradient id={gradId} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor={g.start} stopOpacity={0.25} />
          <stop offset="100%" stopColor={g.start} stopOpacity={0.02} />
        </linearGradient>
      </defs>
    )
  })

  const allDefs = [...gradientDefs, ...areaGradients]

  return (
    <div className="bg-white rounded-xl shadow-sm border border-gray-100 overflow-hidden">
      {/* Header */}
      <div className="flex items-center justify-between px-5 py-4 border-b border-gray-100">
        <div className="flex items-center gap-2">
          <h3 className="font-bold text-gray-800 text-base">Power History</h3>
          {loading && (
            <span className="flex items-center gap-1 text-xs text-gray-400">
              <span className="w-1.5 h-1.5 rounded-full bg-blue-400 animate-pulse" />
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
                  ? 'bg-gradient-to-r from-blue-500 to-indigo-500 text-white shadow-sm'
                  : 'bg-gray-100 text-gray-500 hover:bg-gray-200'
              }`}
            >
              {r}
            </button>
          ))}
        </div>
      </div>

      {/* Legend */}
      {powerKeys.length > 0 && (
        <div className="flex items-center gap-2 px-5 pt-4 pb-2">
          <button
            onClick={toggleAll}
            className="text-xs px-2 py-0.5 rounded border border-gray-200 text-gray-500 hover:bg-gray-50 transition-colors"
          >
            {visibleLines.size === powerKeys.length ? 'Hide all' : 'Show all'}
          </button>
          <div className="w-px h-4 bg-gray-200" />
          {powerKeys.map((k, i) => {
            const g = VC_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]
            const active = visibleLines.has(k)
            return (
              <button
                key={k}
                onClick={() => toggleLine(k)}
                className={`flex items-center gap-1.5 text-xs px-2 py-0.5 rounded-full border transition-all duration-150 ${
                  active
                    ? 'border-transparent shadow-sm'
                    : 'border-gray-200 text-gray-400 opacity-60'
                }`}
                style={active ? { backgroundColor: g.start + '18', borderColor: g.start + '40' } : {}}
              >
                <span
                  className="w-2.5 h-2.5 rounded-full flex-shrink-0"
                  style={{ background: `linear-gradient(135deg, ${g.start}, ${g.end})` }}
                />
                <span style={active ? { color: g.start } : {}}>{keyToLabel(k)}</span>
              </button>
            )
          })}
        </div>
      )}

      {/* Chart */}
      <div className="px-2 pb-4">
        {loading && chartData.length === 0 ? (
          <div className="h-72 flex flex-col items-center justify-center text-gray-300 gap-3">
            <svg className="w-10 h-10 animate-spin" viewBox="0 0 24 24" fill="none">
              <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
              <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
            </svg>
            <span className="text-sm">Fetching power data…</span>
          </div>
        ) : chartData.length === 0 ? (
          <div className="h-72 flex flex-col items-center justify-center text-gray-400 gap-2">
            <svg viewBox="0 0 24 24" className="w-10 h-10 text-gray-300" fill="none" stroke="currentColor" strokeWidth="1.5">
              <path strokeLinecap="round" strokeLinejoin="round" d="M7 12.5l4-4m0 0l4 4m-4-4v12m8-12a9 9 0 11-18 0 9 9 0 0118 0z" />
            </svg>
            <span className="text-sm font-medium">No power data for this range</span>
            <span className="text-xs text-gray-300">Data arrives every 5 seconds</span>
          </div>
        ) : (
          <ResponsiveContainer width="100%" height={300}>
            <AreaChart data={chartData} margin={{ top: 10, right: 16, left: -8, bottom: 0 }}>
              {allDefs}
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
                interval="preserveStartEnd"
              />
              <YAxis
                tick={{ fontSize: 11, fill: '#94a3b8', fontFamily: 'inherit' }}
                axisLine={false}
                tickLine={false}
                unit=" W"
                width={56}
              />
              <Tooltip
                contentStyle={{
                  backgroundColor: '#1e293b',
                  border: 'none',
                  borderRadius: '10px',
                  boxShadow: '0 4px 20px rgba(0,0,0,0.15)',
                  fontSize: 12,
                  color: '#e2e8f0',
                  padding: '8px 12px',
                }}
                labelStyle={{ color: '#94a3b8', fontSize: 11, marginBottom: 4 }}
                itemStyle={{ padding: '2px 0' }}
                formatter={(value: number, name: string) => [`${value?.toFixed(1)} W`, keyToLabel(name)]}
              />
              {visibleKeys.map((k, i) => {
                const g = VC_GRADIENTS[k] ?? FALLBACK_COLORS[i % FALLBACK_COLORS.length]
                const fillId = `area_fill_${k}`
                return (
                  <Area
                    key={k}
                    type="monotone"
                    dataKey={k}
                    stroke={g.start}
                    strokeWidth={2.5}
                    fill={`url(#${fillId})`}
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
              {spikeData.length > 0 && (
                <Scatter
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
      </div>
    </div>
  )
}
