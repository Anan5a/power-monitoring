import { useState, useEffect } from 'react'
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Scatter } from 'recharts'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

interface Props {
  deviceKey: string
}

type Range = '1h' | '6h' | '24h' | '7d' | '30d'

const RANGE_HOURS: Record<Range, number> = { '1h': 1, '6h': 6, '24h': 24, '7d': 168, '30d': 720 }
const RANGE_LIMITS: Record<Range, number> = { '1h': 200, '6h': 200, '24h': 200, '7d': 500, '30d': 500 }

// Find power keys: virtual channels ch0_P/ch1_P/ch2_P + ina226_p
function extractVCPowerKeys(data: TelemetryPoint[]): string[] {
  if (data.length === 0) return []
  const payloadKeys = Object.keys(data[0].payload as Record<string, number>)
  return payloadKeys.filter(k => k.match(/^ch\d_P$/) || k === 'ina226_p')
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

  // Transform data for recharts: [{ time: '12:00', VC0: 12.5, VC1: 0, ... }, ...]
  const chartData = historyData.map(pt => {
    const rec: Record<string, unknown> = { time: new Date(pt.recorded_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) }
    for (const k of powerKeys) {
      rec[k] = (pt.payload as Record<string, number>)[k] ?? null
    }
    return rec
  })

  // Spike dots: mark time of spike on x-axis with y in middle of chart
  const spikeData: Array<{ time: string; spike_y: number }> = []
  for (const pt of historyData) {
    const payload = pt.payload as Record<string, unknown>
    for (let i = 0; i < 3; i++) {
      if (payload[`ina3221_i${i}_spike`] === true) {
        const t = new Date(pt.recorded_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
        spikeData.push({ time: t, spike_y: 0 })
        break
      }
    }
  }

  // VC color map (stable per key)
  const vcColors: Record<string, string> = {
    ina3221_p0: '#3b82f6',
    ina3221_p1: '#22c55e',
    ina3221_p2: '#f59e0b',
    ina226_p: '#a855f7',
    soc_pct0: '#ef4444',
    soc_pct1: '#ef4444',
    soc_pct2: '#ef4444',
    soc_pct3: '#ef4444',
  }
  const colors = ['#3b82f6', '#22c55e', '#f59e0b', '#a855f7', '#ec4899', '#06b6d4']

  const toggleLine = (key: string) => {
    const next = new Set(visibleLines)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    setVisibleLines(next)
  }

  // Simple key to label
  const keyToLabel = (k: string) => {
    if (k === 'ina226_p') return 'INA226'
    return k.replace(/^ch(\d)_P$/i, 'CH$1').toUpperCase()
  }

  return (
    <div className="bg-white rounded-lg shadow p-4">
      <div className="flex items-center justify-between mb-4">
        <h3 className="font-semibold text-gray-700">Power History</h3>
        <div className="flex gap-1">
          {(['1h', '6h', '24h', '7d', '30d'] as Range[]).map(r => (
            <button
              key={r}
              onClick={() => setRange(r)}
              className={`px-3 py-1 rounded text-sm font-medium ${
                range === r ? 'bg-blue-600 text-white' : 'bg-gray-100 text-gray-600 hover:bg-gray-200'
              }`}
            >
              {r}
            </button>
          ))}
        </div>
      </div>

      {loading ? (
        <div className="h-64 flex items-center justify-center text-gray-400">Loading...</div>
      ) : chartData.length === 0 ? (
        <div className="h-64 flex items-center justify-center text-gray-400">No data for this range</div>
      ) : (
        <>
          {/* Legend toggles */}
          <div className="flex flex-wrap gap-3 mb-3">
            {powerKeys.map(k => (
              <button
                key={k}
                onClick={() => toggleLine(k)}
                className={`text-xs px-2 py-1 rounded border ${
                  visibleLines.has(k)
                    ? 'border-blue-400 bg-blue-50 text-blue-700'
                    : 'border-gray-300 text-gray-400'
                }`}
              >
                {keyToLabel(k)}
              </button>
            ))}
          </div>
          <ResponsiveContainer width="100%" height={280}>
            <LineChart data={chartData} margin={{ top: 5, right: 20, left: 0, bottom: 5 }}>
              <CartesianGrid strokeDasharray="3 3" stroke="#f0f0f0" />
              <XAxis dataKey="time" tick={{ fontSize: 11 }} />
              <YAxis tick={{ fontSize: 11 }} unit="W" />
              <Tooltip />
              {powerKeys.map(k => visibleLines.has(k) && (
                <Line
                  key={k}
                  type="monotone"
                  dataKey={k}
                  stroke={vcColors[k] ?? colors[powerKeys.indexOf(k) % colors.length]}
                  strokeWidth={2}
                  dot={false}
                  connectNulls
                />
              ))}
              {spikeData.length > 0 && (
                <Scatter
                  data={spikeData}
                  dataKey="spike_y"
                  line={false}
                  fill="#ef4444"
                />
              )}
            </LineChart>
          </ResponsiveContainer>
        </>
      )}
    </div>
  )
}