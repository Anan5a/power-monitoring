import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

const HISTORY_LIMIT = 200

export function useRealtime(deviceKey: string | null) {
  const [dataPoints, setDataPoints] = useState<TelemetryPoint[]>([])
  const [latestReading, setLatestReading] = useState<TelemetryPoint | null>(null)
  const channelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)

  useEffect(() => {
    if (!deviceKey) return
    if (channelRef.current) {
      supabase.removeChannel(channelRef.current)
      channelRef.current = null
    }
    setLatestReading(null)
    setDataPoints([])

    const channel = supabase
      .channel(`telemetry-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_live',
        filter: `device_id=eq.${deviceKey}`,
      }, payload => {
        const newPoint = payload.new as TelemetryPoint
        // Chart gets raw data (no smoothing)
        setDataPoints(prev => [...prev.slice(-HISTORY_LIMIT), newPoint])
        // Latest reading: use raw payload directly, no EMA
        setLatestReading(newPoint)
      })
      .subscribe()

    channelRef.current = channel
    return () => {
      if (channelRef.current) {
        supabase.removeChannel(channelRef.current)
        channelRef.current = null
      }
    }
  }, [deviceKey])

  return { dataPoints, latestReading }
}