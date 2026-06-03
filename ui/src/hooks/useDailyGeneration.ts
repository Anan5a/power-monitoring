import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'

export interface HourlyBucket {
  hour: string
  value: number
}

export function useDailyGeneration(deviceKey: string | null) {
  const [total, setTotal] = useState(0)
  const [hourly, setHourly] = useState<HourlyBucket[]>([])
  const [isLoading, setIsLoading] = useState(false)
  const mounted = useRef(true)

  useEffect(() => {
    return () => { mounted.current = false }
  }, [])

  useEffect(() => {
    if (!deviceKey) {
      setTotal(0)
      setHourly([])
      setIsLoading(false)
      return
    }
    setIsLoading(true)

    // Use UTC midnight as start
    const startOfDay = new Date()
    startOfDay.setUTCHours(0, 0, 0, 0)

    supabase
      .from('telemetry_computed')
      .select('recorded_at, pv_power')
      .eq('device_key', deviceKey)
      .gte('recorded_at', startOfDay.toISOString())
      .order('recorded_at', { ascending: true })
      .then((response) => {
        if (!mounted.current) return
        setIsLoading(false)
        const rows = response.data
        if (!rows || rows.length === 0) return

        // Group by hour
        const hourBuckets = new Map<string, { sum: number; count: number }>()
        for (const row of rows) {
          const dt = new Date(row.recorded_at)
          const hourKey = `${dt.getUTCHours().toString().padStart(2, '0')}:00`
          if (!hourBuckets.has(hourKey)) {
            hourBuckets.set(hourKey, { sum: 0, count: 0 })
          }
          const bucket = hourBuckets.get(hourKey)!
          bucket.sum += Number(row.pv_power) || 0
          bucket.count += 1
        }

        // Build hourly kWh: avg_power_W * (count_per_hour / 3600) / 1000 = kWh
        // Or simpler: (sum / count) * (count / 3600) / 1000 = sum / 3600 / 1000
        // = sum / 3600000
        const result: HourlyBucket[] = []
        const now = new Date()
        let totalKwh = 0
        for (let h = 0; h <= 23; h++) {
          if (h <= now.getUTCHours()) {
            const key = `${h.toString().padStart(2, '0')}:00`
            const bucket = hourBuckets.get(key)
            if (bucket) {
              const kwh = bucket.sum / 3_600_000
              result.push({ hour: key, value: Math.round(kwh * 100) / 100 })
              totalKwh += kwh
            } else {
              result.push({ hour: key, value: 0 })
            }
          }
        }
        if (!mounted.current) return
        setHourly(result)
        setTotal(Math.round(totalKwh * 100) / 100)
      })
  }, [deviceKey])

  return { total, hourly, isLoading }
}
