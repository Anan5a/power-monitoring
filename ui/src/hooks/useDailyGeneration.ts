import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'
import type { ChannelGroup } from '../lib/types'

export interface HourlyBucket {
  hour: string        // "09:00"
  value: number       // kWh for that hour
}

export function useDailyGeneration(
  deviceKey: string | null,
  channelGroups: ChannelGroup[] | undefined
) {
  const [total, setTotal] = useState(0)
  const [hourly, setHourly] = useState<HourlyBucket[]>([])
  const [isLoading, setIsLoading] = useState(false)

  useEffect(() => {
    if (!deviceKey) { setTotal(0); setHourly([]); return }
    setIsLoading(true)

    // Filter solar channels (icon = 0)
    const solarChannels = channelGroups
      ?.filter(g => g.icon === 0)
      .flatMap(g => {
        const mask = g.channel_mask
        const idxs: number[] = []
        for (let i = 0; i < 4; i++) if (mask & (1 << i)) idxs.push(i)
        return idxs
      }) ?? []

    // Build channel power columns (ch0_P, ch1_P, etc.)
    const pvCols = solarChannels.map(i => `ch${i}_p` as const)

    const startOfDay = new Date()
    startOfDay.setHours(0, 0, 0, 0)
    const startStr = startOfDay.toISOString()

    // Query telemetry_computed directly for hourly PV power per channel
    const selectCols = pvCols.length > 0 ? ['recorded_at', ...pvCols].join(', ') : 'recorded_at'
    supabase
      .from('telemetry_computed')
      .select(selectCols)
      .eq('device_key', deviceKey)
      .gte('recorded_at', startStr)
      .order('recorded_at', { ascending: true })
      .then(({ data: rows, error }) => {
        setIsLoading(false)
        if (error || !rows) return

        // Build hourly buckets
        const buckets = new Map<string, number>() // hour -> sum_wh
        for (const row of rows as unknown as Array<Record<string, unknown>>) {
          const dt = new Date(row.recorded_at as string)
          const hourKey = `${dt.getHours().toString().padStart(2, '0')}:00`
          let pvPower = 0
          for (const col of pvCols) {
            const v = row[col] as number | null
            if (v != null) pvPower += v
          }
          // Each row is ~1-2 seconds; add proportional energy
          const wh = pvPower / 3600  // W * (1/3600) hr = Wh per row
          buckets.set(hourKey, (buckets.get(hourKey) ?? 0) + wh)
        }

        // Convert to array with all hours 00-23 up to current hour
        const result: HourlyBucket[] = []
        const now = new Date()
        let totalKwh = 0
        for (let h = 0; h <= 23; h++) {
          const key = `${h.toString().padStart(2, '0')}:00`
          const wh = buckets.get(key) ?? 0
          const kwh = wh / 1000
          if (h <= now.getHours()) {
            result.push({ hour: key, value: Math.round(kwh * 100) / 100 })
            totalKwh += kwh
          }
        }
        setHourly(result)
        setTotal(Math.round(totalKwh * 100) / 100)
      })
  }, [deviceKey, JSON.stringify(channelGroups)])

  return { total, hourly, isLoading }
}
