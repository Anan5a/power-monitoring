import { useEffect, useState, useRef, useCallback } from 'react'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { computeTelemetry, type ComputedValues } from '../lib/computedTelemetry'
import type { DeviceChannels, TelemetryPoint } from '../lib/types'

interface ComputedRow {
  id: number
  device_key: string
  recorded_at: string
  payload: Record<string, number>
}

const HISTORY_LIMIT = 200
// If no telemetry_computed row within this window, fall back to client compute from telemetry_live
const FALLBACK_AGE_MS = 30_000

export function useComputedTelemetry(deviceKey: string | null) {
  const [computedLatest, setComputedLatest] = useState<ComputedValues | null>(null)
  const [computedHistory, setComputedHistory] = useState<ComputedValues[]>([])
  const [isLoading, setIsLoading] = useState(false)
  const computedChannelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)
  const liveChannelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)
  const deviceChannelsRef = useRef<DeviceChannels | null>(null)
  const lastComputedAtRef = useRef<number | null>(null)

  const recompute = useCallback((payload: Record<string, number>): ComputedValues | null => {
    const channels = deviceChannelsRef.current
    if (!channels) return null
    return computeTelemetry(
      payload,
      channels.channel_groups ?? [],
      channels.battery_profiles ?? []
    )
  }, [])

  useEffect(() => {
    if (!deviceKey) {
      setComputedLatest(null)
      setComputedHistory([])
      lastComputedAtRef.current = null
      return
    }
    setIsLoading(true)

    // Load device channels once for client-side computation
    fetchDeviceChannels(deviceKey).then(dc => {
      deviceChannelsRef.current = dc
      setIsLoading(false)
    })

    // Load recent computed rows (server-side precomputed)
    supabase
      .from('telemetry_computed')
      .select('*')
      .eq('device_key', deviceKey)
      .order('recorded_at', { ascending: false })
      .limit(HISTORY_LIMIT)
      .then(({ data }) => {
        if (data) {
          const rows = (data as ComputedRow[]).slice().reverse()
          const history = rows
            .map(r => recompute(r.payload))
            .filter((v): v is ComputedValues => v !== null)
          setComputedHistory(history)
          if (history.length > 0) {
            setComputedLatest(history[history.length - 1])
            lastComputedAtRef.current = new Date(rows[rows.length - 1].recorded_at).getTime()
          }
        }
      })

    // Subscribe to server-computed telemetry
    const computedChannel = supabase
      .channel(`telemetry-computed-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_computed',
        filter: `device_key=eq.${deviceKey}`,
      }, payload => {
        const row = payload.new as ComputedRow
        const computed = recompute(row.payload)
        if (!computed) return
        lastComputedAtRef.current = new Date(row.recorded_at).getTime()
        setComputedLatest(computed)
        setComputedHistory(prev => {
          const next = [...prev, computed]
          return next.length > HISTORY_LIMIT ? next.slice(-HISTORY_LIMIT) : next
        })
      })
      .subscribe()
    computedChannelRef.current = computedChannel

    // Fallback: subscribe to telemetry_live and compute client-side
    const liveChannel = supabase
      .channel(`telemetry-live-fallback-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_live',
        filter: `device_id=eq.${deviceKey}`,
      }, payload => {
        // Skip if server-computed is fresh
        const last = lastComputedAtRef.current
        if (last && Date.now() - last < FALLBACK_AGE_MS) return
        const point = payload.new as TelemetryPoint
        const computed = recompute(point.payload as Record<string, number>)
        if (!computed) return
        setComputedLatest(computed)
        setComputedHistory(prev => {
          const next = [...prev, computed]
          return next.length > HISTORY_LIMIT ? next.slice(-HISTORY_LIMIT) : next
        })
      })
      .subscribe()
    liveChannelRef.current = liveChannel

    return () => {
      if (computedChannelRef.current) {
        supabase.removeChannel(computedChannelRef.current)
        computedChannelRef.current = null
      }
      if (liveChannelRef.current) {
        supabase.removeChannel(liveChannelRef.current)
        liveChannelRef.current = null
      }
    }
  }, [deviceKey, recompute])

  if (!deviceKey) {
    return { computedLatest: null, computedHistory: [], isLoading: false }
  }

  return { computedLatest, computedHistory, isLoading }
}
