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
  isDaily: boolean
}

function fmtHour(ts: string): string {
  const d = new Date(ts)
  return `${String(d.getHours()).padStart(2, '0')}:00`
}

function fmtDay(ts: string): string {
  const d = new Date(ts)
  const m = String(d.getMonth() + 1).padStart(2, '0')
  const day = String(d.getDate()).padStart(2, '0')
  return `${m}/${day}`
}

export function useDailyGeneration(
  deviceKey: string | null,
  range: DateRange = 'today',
  customStart?: string,
  customEnd?: string
) {
  const [result, setResult] = useState<GenerationResult>({
    total: 0,
    hourly: [],
    rangeLabel: 'Today',
    isDaily: false,
  })
  const [isLoading, setIsLoading] = useState(false)
  const mounted = useRef(true)

  useEffect(() => {
    return () => { mounted.current = false }
  }, [])

  useEffect(() => {
    if (!deviceKey) {
      setResult({ total: 0, hourly: [], rangeLabel: 'Today', isDaily: false })
      setIsLoading(false)
      return
    }
    setIsLoading(true)

    let hours = 24
    let days = 7
    let rangeLabel = ''
    let isDaily = false

    if (range === 'today') {
      hours = 24
      rangeLabel = 'Last 24 Hours'
    } else if (range === 'yesterday') {
      hours = 24
      rangeLabel = 'Yesterday'
    } else if (range === '7d') {
      days = 7
      isDaily = true
      rangeLabel = 'Last 7 Days'
    } else if (range === '30d') {
      days = 30
      isDaily = true
      rangeLabel = 'Last 30 Days'
    } else if (range === 'custom' && customStart && customEnd) {
      const start = new Date(customStart + 'T00:00:00')
      const end = new Date(customEnd + 'T23:59:59')
      const diffMs = end.getTime() - start.getTime()
      const diffHrs = Math.ceil(diffMs / 3600000)
      const diffDays = Math.ceil(diffMs / 86400000)
      if (diffHrs > 48) {
        days = Math.max(1, diffDays)
        isDaily = true
      } else {
        hours = Math.max(1, diffHrs)
      }
      rangeLabel = `${customStart} → ${customEnd}`
    } else {
      setIsLoading(false)
      return
    }

    if (isDaily) {
      supabase
        .rpc('get_daily_generation', { p_device_key: deviceKey, p_days: days })
        .then(({ data, error }) => {
          if (!mounted.current) return
          setIsLoading(false)
          if (error || !Array.isArray(data) || data.length === 0) {
            setResult({ total: 0, hourly: [], rangeLabel, isDaily })
            return
          }

          let totalKwh = 0
          const buckets: HourlyBucket[] = []

          for (const row of data) {
            const kwh = row.kwh ?? 0
            const label = fmtDay(row.day)
            buckets.push({
              hour: label,
              value: Math.round(kwh * 100) / 100,
              projected: row.is_partial ?? false,
            })
            totalKwh += kwh
          }

          buckets.reverse()

          setResult({
            total: Math.round(totalKwh * 100) / 100,
            hourly: buckets,
            rangeLabel,
            isDaily,
          })
        })
    } else {
      supabase
        .rpc('get_hourly_generation', { p_device_key: deviceKey, p_hours: hours })
        .then(({ data, error }) => {
          if (!mounted.current) return
          setIsLoading(false)
          if (error || !Array.isArray(data) || data.length === 0) {
            setResult({ total: 0, hourly: [], rangeLabel, isDaily })
            return
          }

          let totalKwh = 0
          const buckets: HourlyBucket[] = []

          for (const row of data) {
            const kwh = row.kwh ?? 0
            const timeLabel = fmtHour(row.hour_start)
            buckets.push({
              hour: timeLabel,
              value: Math.round(kwh * 100) / 100,
              projected: row.is_partial ?? false,
            })
            totalKwh += kwh
          }

          buckets.reverse()

          setResult({
            total: Math.round(totalKwh * 100) / 100,
            hourly: buckets,
            rangeLabel,
            isDaily,
          })
        })
    }
  }, [deviceKey, range, customStart, customEnd])

  return { ...result, isLoading }
}
