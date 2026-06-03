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

    const startOfDay = new Date()
    startOfDay.setHours(0, 0, 0, 0)

    supabase
      .from('telemetry_computed')
      .select('recorded_at, pv_power')
      .eq('device_key', deviceKey)
      .gte('recorded_at', startOfDay.toISOString())
      .order('recorded_at', { ascending: true })
      .then((response) => {
        const rows = response.data
        console.log('DailyGen rows:', rows?.length, 'error:', response.error)
        if (!mounted.current) return
        setIsLoading(false)
        if (response.error || !rows || rows.length === 0) {
          console.log('No data or error:', response.error)
          return
        }

        // Debug: check first row
        if (rows[0]) {
          console.log('First row:', rows[0])
        }

        const buckets = new Map<string, number>()
        let debugCount = 0
        for (const row of rows) {
          const dt = new Date(row.recorded_at)
          const hourKey = `${dt.getHours().toString().padStart(2, '0')}:00`
          const pvPower = Number(row.pv_power) || 0  // explicit number conversion
          const wh = pvPower / 3600
          buckets.set(hourKey, (buckets.get(hourKey) ?? 0) + wh)
          debugCount++
        }
        console.log('Buckets debug:', Object.fromEntries(buckets), 'count:', debugCount)

        const result: HourlyBucket[] = []
        const now = new Date()
        let totalKwh = 0
        for (let h = 0; h <= 23; h++) {
          if (h <= now.getHours()) {
            const key = `${h.toString().padStart(2, '0')}:00`
            const kwh = buckets.get(key) ?? 0
            result.push({ hour: key, value: Math.round(kwh * 100) / 100 })
            totalKwh += kwh
          }
        }
        console.log('totalKwh:', totalKwh, 'now.getHours():', now.getHours())
        if (!mounted.current) return
        setHourly(result)
        setTotal(Math.round(totalKwh * 100) / 100)
      })
  }, [deviceKey])

  return { total, hourly, isLoading }
}