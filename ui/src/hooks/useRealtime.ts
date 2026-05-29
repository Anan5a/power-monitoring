import { useEffect, useState, useRef, useCallback } from 'react'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

// EMA alpha — higher = more responsive, lower = smoother
const EMA = 0.3
// Reasonable physical limits per channel — values outside these are dropped as noise
const MAX_V_PER_CHANNEL = 200   // V  (covers 48V battery + margin)
const MAX_I_PER_CHANNEL = 100   // A
const MAX_P_PER_CHANNEL = 5000  // W

function isAbsurd(key: string, val: number): boolean {
  const k = key.toLowerCase()
  if (k.includes('_v') || k.includes('volt')) return val < 0 || val > MAX_V_PER_CHANNEL
  if (k.includes('_i') || k.includes('curr')) return val < 0 || val > MAX_I_PER_CHANNEL
  if (k.includes('_p') || k.includes('watt') || k.includes('power')) return val < 0 || val > MAX_P_PER_CHANNEL
  return false
}

function applyEMA(
  reading: Record<string, number>,
  prev: Record<string, number>
): Record<string, number> {
  const next: Record<string, number> = {}
  for (const k of Object.keys(reading)) {
    const raw = reading[k]
    const prevVal = prev[k] ?? raw
    next[k] = isAbsurd(k, raw) && prevVal !== 0
      ? prevVal   // absurd new reading — keep previous, don't corrupt EMA state
      : prevVal + EMA * (raw - prevVal)
  }
  // Don't carry forward stale keys — each payload defines the current value set
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
    emaRef.current = applyEMA(raw, emaRef.current)
    // Chart gets raw data; cards get smoothed data
    setDataPoints(prev => [...prev.slice(-199), newPoint])
    setLatestReading({ ...newPoint, payload: { ...emaRef.current } })
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