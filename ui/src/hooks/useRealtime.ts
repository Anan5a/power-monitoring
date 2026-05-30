import { useEffect, useState, useRef, useCallback } from 'react'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

// EMA alpha — higher = more responsive, lower = smoother
const EMA = 0.7
// Reasonable physical limits per channel — values outside these are dropped as noise
const MAX_V_PER_CHANNEL = 500   // V  (covers 48V battery + margin)
const MAX_I_PER_CHANNEL = 300   // A
const MAX_P_PER_CHANNEL = 20000  // W

function isAbsurd(key: string, val: number): boolean {
  const k = key.toLowerCase()
  if (k.includes('_v') || k.includes('volt')) return val < 0 || val > MAX_V_PER_CHANNEL
  if (k.includes('_i') || k.includes('curr')) return val < 0 || val > MAX_I_PER_CHANNEL
  if (k.includes('_p') || k.includes('watt') || k.includes('power')) return val < 0 || val > MAX_P_PER_CHANNEL
  return false
}

function smoothEMA(
  raw: Record<string, number>,
  prev: Record<string, number>
): Record<string, number> {
  const next = { ...prev }
  for (const k of Object.keys(raw)) {
    const v = raw[k]
    const p = prev[k] ?? v
    next[k] = isAbsurd(k, v) && p !== 0 ? p : p + EMA * (v - p)
  }
  return next
}

export function useRealtime(deviceKey: string | null) {
  const [dataPoints, setDataPoints] = useState<TelemetryPoint[]>([])
  const [latestReading, setLatestReading] = useState<TelemetryPoint | null>(null)
  const emaRef = useRef<Record<string, number>>({})
  const channelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)

  const handlePayload = useCallback((payload: { new: TelemetryPoint }) => {
    const newPoint = payload.new as TelemetryPoint
    const raw = newPoint.payload as Record<string, number>
    emaRef.current = smoothEMA(raw, emaRef.current)
    // Chart gets raw data; cards get EMA-smoothed data
    setDataPoints(prev => [...prev.slice(-199), newPoint])
    // Use EMA state directly — persists all keys even if a new payload skips them
    setLatestReading(prev => prev
      ? { ...prev, recorded_at: newPoint.recorded_at, payload: { ...emaRef.current } }
      : { ...newPoint, payload: { ...emaRef.current } }
    )
  }, [])

  useEffect(() => {
    if (!deviceKey) return
    if (channelRef.current) {
      supabase.removeChannel(channelRef.current)
      channelRef.current = null
    }
    emaRef.current = {}
    setLatestReading(null)
    setDataPoints([])

    const channel = supabase
      .channel(`telemetry-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_live',
        filter: `device_id=eq.${deviceKey}`,
      }, handlePayload)
      .subscribe()

    channelRef.current = channel
    return () => {
      if (channelRef.current) {
        supabase.removeChannel(channelRef.current)
        channelRef.current = null
      }
    }
  }, [deviceKey, handlePayload])

  return { dataPoints, latestReading }
}