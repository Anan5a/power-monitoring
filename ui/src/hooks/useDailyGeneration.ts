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
    if (!deviceKey) { setTotal(0); setHourly([]); setIsLoading(false); return }
    setIsLoading(true)

    const startOfDay = new Date()
    startOfDay.setHours(0, 0, 0, 0)

    supabase
      .from('telemetry_computed')
      .select('recorded_at, pv_power')
      .eq('device_key', deviceKey)
      .gte('recorded_at', startOfDay.toISOString())
      .order('recorded_at', { ascending: true })
      .then(({ data: rows, error }) => {
        if (!mounted.current) return
        setIsLoading(false)
        if (error || !rows) return

        const buckets = new Map<string, number>()
        for (const row of rows) {
          const dt = new Date(row.recorded_at)
          const hourKey = `${dt.getHours().toString().padStart(2, '0')}:00`
          const pvPower = row.pv_power ?? 0
          const wh = pvPower / 3600
          buckets.set(hourKey, (buckets.get(hourKey) ?? 0) + wh)
        }

        const result: HourlyBucket[] = []
        const now = new Date()
        let totalKwh = 0
        for (let h = 0; h <= 23; h++) {
          if (h <= now.getHours()) {
            const key = `${h.toString().padStart(2, '0')}:00`
            const kwh = (buckets.get(key) ?? 0) / 1000
            result.push({ hour: key, value: Math.round(kwh * 100) / 100 })
            totalKwh += kwh
          }
        }
        if (!mounted.current) return
        setHourly(result)
        setTotal(Math.round(totalKwh * 100) / 100)
      })
  }, [deviceKey])

  return { total, hourly, isLoading }
}
