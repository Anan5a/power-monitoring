import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'

export interface HourlyBucket {
  hour: string
  value: number
  projected?: boolean
}

export type DateRange = 'today' | 'yesterday' | '7d' | '30d' | 'custom'

export interface GenerationResult {
  total: number
  hourly: HourlyBucket[]
  rangeLabel: string
}

function fmtHour(ts: string): string {
  const d = new Date(ts)
  return `${String(d.getHours()).padStart(2, '0')}:00`
}

export function useDailyGeneration(
  deviceKey: string | null,
  range: DateRange = 'today',
  customStart?: string,
  customEnd?: string
) {
  const [result, setResult] = useState<GenerationResult>({ total: 0, hourly: [], rangeLabel: 'Today' })
  const [isLoading, setIsLoading] = useState(false)
  const mounted = useRef(true)

  useEffect(() => {
    return () => { mounted.current = false }
  }, [])

  useEffect(() => {
    if (!deviceKey) {
      setResult({ total: 0, hourly: [], rangeLabel: 'Today' })
      setIsLoading(false)
      return
    }
    setIsLoading(true)

    const now = new Date()
    let hours = 24
    let rangeLabel = ''

    if (range === 'today') {
      hours = now.getHours() + 1  // midnight through current hour
      rangeLabel = 'Today'
    } else if (range === 'yesterday') {
      hours = 24
      rangeLabel = 'Yesterday'
    } else if (range === '7d') {
      hours = 24 * 7
      rangeLabel = 'Last 7 Days'
    } else if (range === '30d') {
      hours = 24 * 30
      rangeLabel = 'Last 30 Days'
    } else if (range === 'custom' && customStart && customEnd) {
      const start = new Date(customStart + 'T00:00:00')
      const end = new Date(customEnd + 'T23:59:59')
      const diffMs = end.getTime() - start.getTime()
      hours = Math.max(1, Math.ceil(diffMs / 3600000))
      rangeLabel = `${customStart} → ${customEnd}`
    } else {
      setIsLoading(false)
      return
    }

    supabase
      .rpc('get_hourly_generation', { p_device_key: deviceKey, p_hours: hours })
      .then(({ data, error }) => {
        if (!mounted.current) return
        setIsLoading(false)
        if (error || !Array.isArray(data) || data.length === 0) {
          setResult({ total: 0, hourly: [], rangeLabel })
          return
        }

        let totalKwh = 0
        const hourly: HourlyBucket[] = []

        for (const row of data) {
          const kwh = row.kwh ?? 0
          const timeLabel = fmtHour(row.hour_start)
          hourly.push({
            hour: timeLabel,
            value: Math.round(kwh * 100) / 100,
            projected: row.is_partial ?? false,
          })
          totalKwh += kwh
        }

        // get_hourly_generation returns newest first; reverse for chart
        hourly.reverse()

        setResult({
          total: Math.round(totalKwh * 100) / 100,
          hourly,
          rangeLabel,
        })
      })
  }, [deviceKey, range, customStart, customEnd])

  return { ...result, isLoading }
}
