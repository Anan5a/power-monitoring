import { memo, useEffect, useState } from 'react'
import { SunIcon } from '@heroicons/react/24/outline'
import { supabase } from '../lib/supabase'

interface Props {
  deviceId: string
}

interface DailyResult {
  total: number
  hourly: Array<{ hour: string; value: number; projected?: boolean }>
  rangeLabel: string
}

function GenerationWidget({ deviceId }: Props) {
  const [range, setRange] = useState<'today' | 'yesterday' | '7d' | '30d'>('today')
  const [result, setResult] = useState<DailyResult>({ total: 0, hourly: [], rangeLabel: 'Today' })
  const [isLoading, setIsLoading] = useState(false)

  useEffect(() => {
    if (!deviceId) return
    let cancelled = false
    setIsLoading(true)
    const now = new Date()
    let startTime: Date
    let endTime: Date
    let rangeLabel = ''
    let isDaily = false
    if (range === 'today') {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()))
      endTime = now; rangeLabel = 'Today'
    } else if (range === 'yesterday') {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1))
      endTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1, 23, 59, 59, 999))
      rangeLabel = 'Yesterday'
    } else if (range === '7d') {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 6))
      endTime = now; isDaily = true; rangeLabel = 'Last 7 Days'
    } else {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 29))
      endTime = now; isDaily = true; rangeLabel = 'Last 30 Days'
    }
    const fn = isDaily ? 'get_daily_generation' : 'get_hourly_generation'
    supabase.rpc(fn, {
      p_device_key: deviceId,
      p_start_time: startTime.toISOString(),
      p_end_time: endTime.toISOString(),
    }).then(({ data, error }) => {
      if (cancelled) return
      setIsLoading(false)
      if (error || !Array.isArray(data) || data.length === 0) {
        setResult({ total: 0, hourly: [], rangeLabel })
        return
      }
      let total = 0
      const buckets = (data as any[]).map((row) => {
        const kwh = row.kwh ?? 0
        const label = isDaily
          ? `${String(new Date(row.day).getMonth() + 1).padStart(2, '0')}/${String(new Date(row.day).getDate()).padStart(2, '0')}`
          : `${String(new Date(row.hour_start).getHours()).padStart(2, '0')}:00`
        total += kwh
        return { hour: label, value: Math.round(kwh * 100) / 100, projected: row.is_partial ?? false }
      }).reverse()
      setResult({ total: Math.round(total * 100) / 100, hourly: buckets, rangeLabel })
    })
    return () => { cancelled = true }
  }, [deviceId, range])

  return (
    <div className="h-full w-full bg-gradient-to-br from-amber-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-5">
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <SunIcon className="w-5 h-5 text-amber-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Generation</span>
        </div>
        <span className="text-xs text-amber-600 font-medium">kWh</span>
      </div>
      <div className="flex items-center gap-1 mb-3 flex-wrap">
        {(['today', 'yesterday', '7d', '30d'] as const).map(r => (
          <button key={r} onClick={() => setRange(r)}
            className={`px-2.5 py-1 rounded-lg text-[11px] font-semibold transition-colors duration-150 ${range === r ? 'bg-amber-500 text-white shadow-sm' : 'bg-slate-100 text-slate-500 hover:bg-slate-200'}`}>
            {r === '7d' ? '7 Days' : r === '30d' ? '30 Days' : r === 'today' ? 'Today' : 'Yesterday'}
          </button>
        ))}
      </div>
      <div className="flex items-baseline gap-1.5 mb-1 min-h-[36px]">
        {isLoading ? (
          <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
        ) : (
          <>
            <span className="text-3xl font-bold text-amber-600 tabular-nums">{result.total > 0 ? result.total.toFixed(2) : '0.00'}</span>
            <span className="text-base font-medium text-amber-500">kWh</span>
          </>
        )}
      </div>
      <div className="text-[10px] text-slate-400">{result.rangeLabel}</div>
    </div>
  )
}

export default memo(GenerationWidget)
