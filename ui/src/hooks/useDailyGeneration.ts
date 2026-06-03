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

    // Determine date boundaries
    const now = new Date()
    let startDate: Date
    let endDate: Date
    let rangeLabel = ''

    if (range === 'today') {
      startDate = new Date(now.getFullYear(), now.getMonth(), now.getDate())
      endDate = now
      rangeLabel = 'Today'
    } else if (range === 'yesterday') {
      const yesterday = new Date(now)
      yesterday.setDate(yesterday.getDate() - 1)
      startDate = new Date(yesterday.getFullYear(), yesterday.getMonth(), yesterday.getDate())
      endDate = new Date(yesterday.getFullYear(), yesterday.getMonth(), yesterday.getDate(), 23, 59, 59)
      rangeLabel = 'Yesterday'
    } else if (range === '7d') {
      startDate = new Date(now)
      startDate.setDate(startDate.getDate() - 7)
      endDate = now
      rangeLabel = 'Last 7 Days'
    } else if (range === '30d') {
      startDate = new Date(now)
      startDate.setDate(startDate.getDate() - 30)
      endDate = now
      rangeLabel = 'Last 30 Days'
    } else if (range === 'custom' && customStart && customEnd) {
      startDate = new Date(customStart + 'T00:00:00')
      endDate = new Date(customEnd + 'T23:59:59')
      rangeLabel = `${customStart} → ${customEnd}`
    } else {
      setIsLoading(false)
      return
    }

    const startStr = startDate.toISOString()
    const endStr = endDate.toISOString()

    supabase
      .from('telemetry_computed')
      .select('recorded_at, energy_wh1')
      .eq('device_key', deviceKey)
      .gte('recorded_at', startStr)
      .lte('recorded_at', endStr)
      .order('recorded_at', { ascending: true })
      .then(({ data, error }) => {
        if (!mounted.current) return
        setIsLoading(false)
        if (error || !data || data.length === 0) {
          if (!mounted.current) return
          setResult({ total: 0, hourly: [], rangeLabel })
          return
        }

        // Group by hour buckets
        const hourlyMap = new Map<string, { first: number; max: number }>()

        for (const row of data) {
          if (row.energy_wh1 == null) continue
          const d = new Date(row.recorded_at)
          const hourKey = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:00`

          if (!hourlyMap.has(hourKey)) {
            hourlyMap.set(hourKey, { first: row.energy_wh1, max: row.energy_wh1 })
          } else {
            const entry = hourlyMap.get(hourKey)!
            entry.first = entry.first  // keep first
            entry.max = Math.max(entry.max, row.energy_wh1)
          }
        }

        const nowLocal = new Date()
        const isPartialHour = (hour: string) => {
          const [dateStr, timeStr] = hour.split(' ')
          const [y, m, d] = dateStr.split('-').map(Number)
          const [h] = timeStr.split(':').map(Number)
          const bucketEnd = new Date(y, m - 1, d, h + 1)
          return bucketEnd > nowLocal
        }

        const result: HourlyBucket[] = []
        let totalKwh = 0

        for (const [hourKey, entry] of hourlyMap) {
          const delta = Math.max(0, entry.max - entry.first) / 1000
          const timeStr = hourKey.split(' ')[1]
          const hourLabel = timeStr

          const partial = isPartialHour(hourKey)
          result.push({ hour: hourLabel, value: Math.round(delta * 100) / 100, projected: partial })
          totalKwh += delta
        }

        result.sort((a, b) => a.hour.localeCompare(b.hour))

        if (!mounted.current) return
        setResult({
          total: Math.round(totalKwh * 100) / 100,
          hourly: result,
          rangeLabel,
        })
      })
  }, [deviceKey, range, customStart, customEnd])

  return { ...result, isLoading }
}