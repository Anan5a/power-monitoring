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

    const today = new Date()
    today.setUTCHours(0, 0, 0, 0)
    const dateStr = today.toISOString().split('T')[0]

    supabase
      .rpc('get_hourly_pv_generation', {
        p_device_key: deviceKey,
        p_date: dateStr
      })
      .then((response) => {
        if (!mounted.current) return
        setIsLoading(false)
        if (response.error || !response.data) {
          console.error('RPC error:', response.error)
          return
        }

        const result: HourlyBucket[] = []
        let totalKwh = 0
        const now = new Date()

        for (let h = 0; h <= 23; h++) {
          if (h <= now.getUTCHours()) {
            const key = `${h.toString().padStart(2, '0')}:00`
            const row = response.data.find(r => {
              const hour = new Date(r.hour).getUTCHours()
              return hour === h
            })
            const kwh = row ? Number(row.kwh) : 0
            result.push({ hour: key, value: kwh })
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