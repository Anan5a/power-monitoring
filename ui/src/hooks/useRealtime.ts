import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

export function useRealtime(deviceKey: string | null) {
  const [dataPoints, setDataPoints] = useState<TelemetryPoint[]>([])
  const [latestReading, setLatestReading] = useState<TelemetryPoint | null>(null)

  useEffect(() => {
    if (!deviceKey) return

    const channel = supabase
      .channel(`telemetry-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_live',
        filter: `device_id=eq.${deviceKey}`,
      }, (payload) => {
        const newPoint = payload.new as TelemetryPoint
        setLatestReading(newPoint)
        setDataPoints(prev => [...prev.slice(-199), newPoint])
      })
      .subscribe()

    return () => {
      supabase.removeChannel(channel)
    }
  }, [deviceKey])

  return { dataPoints, latestReading }
}