import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'

export interface HourlyBucket {
  hour: string
  value: number
  projected?: boolean
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

    // Get local date string (YYYY-MM-DD in browser timezone)
    const now = new Date()
    const localDate = now.toLocaleDateString('en-CA')

    supabase
      .rpc('get_hourly_pv_generation', {
        p_device_key: deviceKey,
        p_date: localDate
      })
      .then((response) => {
        if (!mounted.current) return
        setIsLoading(false)
        if (response.error || !response.data) return

        const result: HourlyBucket[] = []
        let totalKwh = 0
        const currentLocalHour = now.getHours()

        for (let h = 0; h <= 23; h++) {
          if (h <= currentLocalHour) {
            const key = `${h.toString().padStart(2, '0')}:00`

            // Find UTC bucket that corresponds to this local hour
            const row = response.data.find((r: { hour: string; kwh: string }) => {
              const utcDt = new Date(r.hour)
              const localHour = utcDt.getHours()
              return localHour === h
            })

            let kwh = row ? Number(row.kwh) : 0

            // For current hour, project full hour kWh based on elapsed time
            if (h === currentLocalHour && row) {
              const minuteOfHour = now.getMinutes()
              const elapsedRatio = minuteOfHour > 0 ? 60 / minuteOfHour : 1
              kwh = kwh * elapsedRatio
              result.push({ hour: key, value: Math.round(kwh * 100) / 100, projected: true })
            } else {
              result.push({ hour: key, value: Math.round(kwh * 100) / 100 })
            }
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