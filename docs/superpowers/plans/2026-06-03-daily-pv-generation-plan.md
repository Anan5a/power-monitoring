# Daily PV Generation + Dashboard Fixes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a daily PV generation card with sparkline, fix QuickStatsRow mobile overflow, show last known data when realtime disconnects.

**Architecture:** New hook + card component. Existing useRealtime gets a stale-data ref. QuickStatsRow gets overflow handling and stale indicator.

**Tech Stack:** React, TypeScript, Tailwind, Supabase, recharts (already used in PowerHistoryChart).

---

## File Map

- Create: `ui/src/hooks/useDailyGeneration.ts`
- Create: `ui/src/components/DailyGenerationCard.tsx`
- Modify: `ui/src/hooks/useRealtime.ts` — add stale data tracking
- Modify: `ui/src/components/QuickStatsRow.tsx` — mobile overflow + stale display
- Modify: `ui/src/pages/DashboardPage.tsx` — add DailyGenerationCard

---

### Task 1: `useDailyGeneration` hook

**Files:**
- Create: `ui/src/hooks/useDailyGeneration.ts`

```typescript
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
    // Sum expression
    const sumExpr = pvCols.length > 0
      ? `(${pvCols.join(' + ')})`
      : '0'

    const startOfDay = new Date()
    startOfDay.setHours(0, 0, 0, 0)
    const startStr = startOfDay.toISOString()

    // Query hourly average PV power
    // Each row = avg power in that hour; kWh = avg_w * 1hr / 1000
    supabase.rpc('get_aggregated_telemetry', {
      p_device_key: deviceKey,
      p_hours: 24,
      p_metric: 'power',
    }).then(({ data, error }) => {
      if (error || !data) { setIsLoading(false); return }

      // Filter rows for today only
      const today = new Date()
      today.setHours(0, 0, 0, 0)
      const todayMs = today.getTime()

      const hourlyMap = new Map<string, number>()

      // We need per-channel hourly data. The RPC aggregates all ch*_P into one series.
      // Instead, query telemetry_computed directly for hourly PV power per channel.
      supabase
        .from('telemetry_computed')
        .select(`recorded_at, ${pvCols.map(c => c).join(', ')}`)
        .eq('device_key', deviceKey)
        .gte('recorded_at', startStr)
        .order('recorded_at', { ascending: true })
        .then(({ data: rows, error: err2 }) => {
          setIsLoading(false)
          if (err2 || !rows) return

          // Build hourly buckets
          const buckets = new Map<string, number>() // hour -> sum_wh
          for (const row of rows as Array<Record<string, unknown>>) {
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

          // Convert to array with all hours 00-23
          const result: HourlyBucket[] = []
          const now = new Date()
          let totalKwh = 0
          for (let h = 0; h <= 23; h++) {
            const key = `${h.toString().padStart(2, '0')}:00`
            const wh = buckets.get(key) ?? 0
            const kwh = wh / 1000
            // Only show hours up to current hour
            if (h <= now.getHours()) {
              result.push({ hour: key, value: Math.round(kwh * 100) / 100 })
              totalKwh += kwh
            }
          }
          setHourly(result)
          setTotal(Math.round(totalKwh * 100) / 100)
        })
    })
  }, [deviceKey, JSON.stringify(channelGroups)])

  return { total, hourly, isLoading }
}
```

---

### Task 2: `DailyGenerationCard` component

**Files:**
- Create: `ui/src/components/DailyGenerationCard.tsx`

```tsx
import { motion, AnimatePresence } from 'framer-motion'
import { SunIcon } from '@heroicons/react/24/outline'
import { AreaChart, Area, ResponsiveContainer, Tooltip } from 'recharts'
import { useDailyGeneration } from '../hooks/useDailyGeneration'
import type { DeviceChannels } from '../lib/types'

interface Props {
  deviceKey: string
  deviceChannels?: DeviceChannels | null
}

export default function DailyGenerationCard({ deviceKey, deviceChannels }: Props) {
  const { total, hourly, isLoading } = useDailyGeneration(
    deviceKey,
    deviceChannels?.channel_groups
  )
  const solarChannels = deviceChannels?.channel_groups?.filter(g => g.icon === 0) ?? []
  const hasSolar = solarChannels.length > 0

  // Format hour labels for display (show only meaningful hours)
  const chartData = hourly.map(h => ({
    time: h.hour,
    kWh: h.value,
  }))

  if (!hasSolar) {
    return (
      <div className="bg-gradient-to-br from-slate-50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
        <div className="flex items-center gap-2 mb-3">
          <SunIcon className="w-5 h-5 text-amber-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Today's Generation</span>
        </div>
        <div className="text-slate-400 text-sm">No solar channels configured</div>
      </div>
    )
  }

  return (
    <div className="bg-gradient-to-br from-amber-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
      <div className="flex items-start justify-between mb-3">
        <div className="flex items-center gap-2">
          <SunIcon className="w-5 h-5 text-amber-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Today's Generation</span>
        </div>
        <span className="text-xs text-amber-600 font-medium">kWh</span>
      </div>

      <AnimatePresence mode="popLayout" initial={false}>
        <motion.div
          key={total}
          initial={{ y: 8, opacity: 0 }}
          animate={{ y: 0, opacity: 1 }}
          exit={{ y: -8, opacity: 0 }}
          transition={{ duration: 0.3 }}
          className="flex items-baseline gap-1.5 mb-4"
        >
          {isLoading ? (
            <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
          ) : (
            <>
              <span className="text-4xl font-bold text-amber-600 tabular-nums">
                {total > 0 ? total.toFixed(2) : '0.00'}
              </span>
              <span className="text-lg font-medium text-amber-500">kWh</span>
            </>
          )}
        </motion.div>
      </AnimatePresence>

      {/* Sparkline */}
      {chartData.length > 0 && (
        <div className="h-16">
          <ResponsiveContainer width="100%" height="100%">
            <AreaChart data={chartData} margin={{ top: 2, right: 0, left: 0, bottom: 0 }}>
              <defs>
                <linearGradient id="gradGen" x1="0" y1="0" x2="0" y2="1">
                  <stop offset="5%" stopColor="#f59e0b" stopOpacity={0.4} />
                  <stop offset="95%" stopColor="#f59e0b" stopOpacity={0.05} />
                </linearGradient>
              </defs>
              <Tooltip
                formatter={(v: number) => [`${v.toFixed(3)} kWh`, 'Generation']}
                labelFormatter={(l: string) => l}
                contentStyle={{ fontSize: 11, padding: '2px 6px' }}
              />
              <Area
                type="monotone"
                dataKey="kWh"
                stroke="#f59e0b"
                strokeWidth={1.5}
                fill="url(#gradGen)"
                dot={false}
                connectNulls
              />
            </AreaChart>
          </ResponsiveContainer>
        </div>
      )}

      {chartData.length > 0 && (
        <div className="text-[10px] text-slate-400 mt-1 text-right">
          00:00 → {chartData[chartData.length - 1]?.hour ?? 'now'}
        </div>
      )}
    </div>
  )
}
```

---

### Task 3: Fix QuickStatsRow mobile overflow

**Files:**
- Modify: `ui/src/components/QuickStatsRow.tsx:113`

Find the wrapping `<div>` at line ~113:

```tsx
<div className="flex flex-wrap items-center justify-between gap-x-6 gap-y-4">
```

Change to:

```tsx
<div className="flex flex-wrap items-center justify-between gap-x-6 gap-y-4 overflow-x-auto pb-1 -mx-1 px-1">
```

This enables horizontal scroll on very small screens. The `-mx-1 px-1` hides the scrollbar offset.

---

### Task 4: Stale data display in useRealtime + QuickStatsRow

**Files:**
- Modify: `ui/src/hooks/useRealtime.ts:85-131`
- Modify: `ui/src/components/QuickStatsRow.tsx:101-106`

**In `useRealtime.ts`:**

Add a `lastSeenAt` ref and expose `isStale` boolean:

```typescript
export function useRealtime(deviceKey: string | null) {
  const [dataPoints, setDataPoints] = useState<TelemetryPoint[]>([])
  const [latestReading, setLatestReading] = useState<TelemetryPoint | null>(null)
  const [isStale, setIsStale] = useState(false)
  const channelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)
  const staleTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  function scheduleStaleTimer() {
    if (staleTimerRef.current) clearTimeout(staleTimerRef.current)
    staleTimerRef.current = setTimeout(() => setIsStale(true), 15000)
  }

  useEffect(() => {
    if (!deviceKey) return
    if (channelRef.current) {
      supabase.removeChannel(channelRef.current)
      channelRef.current = null
    }
    setLatestReading(null)
    setDataPoints([])
    setIsStale(false)

    const channel = supabase
      .channel(`telemetry-${deviceKey}`)
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: 'telemetry_computed',
        filter: `device_key=eq.${deviceKey}`,
      }, payload => {
        const row = payload.new as ComputedRow
        const point: TelemetryPoint = {
          id: row.id,
          device_id: row.device_key,
          recorded_at: row.recorded_at,
          payload: computedToPayload(row),
          metadata: {},
        }
        setDataPoints(prev => [...prev.slice(-HISTORY_LIMIT), point])
        setLatestReading(point)
        setIsStale(false)
        scheduleStaleTimer()
      })
      .subscribe()

    channelRef.current = channel
    scheduleStaleTimer()
    return () => {
      if (channelRef.current) {
        supabase.removeChannel(channelRef.current)
        channelRef.current = null
      }
      if (staleTimerRef.current) clearTimeout(staleTimerRef.current)
    }
  }, [deviceKey])

  return { dataPoints, latestReading, isStale }
}
```

**In `QuickStatsRow.tsx`:**

Add `isStale` prop:

```typescript
interface Props {
  latestReading: Record<string, number> | null
  deviceChannels: DeviceChannels | null
  relayOn: boolean[]
  isStale?: boolean
}
```

Add stale indicator styling. In the outer div (line ~113), add conditional class:

```tsx
<div className={`bg-white rounded-2xl shadow-sm border border-slate-100 px-6 py-4 mb-6 ${isStale ? 'opacity-60' : ''}`}>
```

And add a small "offline" or "stale" badge at the bottom:

```tsx
{isStale && (
  <div className="mt-3 pt-3 border-t border-slate-100">
    <span className="text-[10px] text-slate-400 italic">Data may be stale — realtime disconnected</span>
  </div>
)}
```

**In `DashboardPage.tsx`:**

```tsx
const { latestReading, isStale } = useRealtime(selectedDevice?.device_key ?? null)
// ...
<QuickStatsRow
  latestReading={latestReading?.payload as Record<string, number> ?? null}
  deviceChannels={deviceChannels}
  relayOn={[false, false, false, false]}
  isStale={isStale}
/>
```

---

### Task 5: Dashboard integration

**Files:**
- Modify: `ui/src/pages/DashboardPage.tsx:134`

Add `DailyGenerationCard` to the top grid:

```tsx
{/* Top metrics row */}
<div className="grid grid-cols-1 md:grid-cols-3 gap-4">
  <DailyGenerationCard deviceKey={selectedDevice.device_key} deviceChannels={deviceChannels} />
  <InverterPowerCard inverterPower={computed.inverter_power} systemStatus={computed.system_status} />
  <RelaySwitchRow deviceKey={selectedDevice.device_key} />
</div>
```

---

## Summary

| Task | File | Key change |
|------|------|-----------|
| 1 | `ui/src/hooks/useDailyGeneration.ts` | New hook, queries telemetry_computed for today's PV power by hour |
| 2 | `ui/src/components/DailyGenerationCard.tsx` | New card, amber solar theme, kWh display + recharts sparkline |
| 3 | `ui/src/components/QuickStatsRow.tsx` | Add `overflow-x-auto` for mobile |
| 4 | `ui/src/hooks/useRealtime.ts` + `QuickStatsRow.tsx` | Add `lastSeenAt` tracking, stale indicator |
| 5 | `ui/src/pages/DashboardPage.tsx` | Add DailyGenerationCard to layout, pass stale props |

**Order:** 4 → 2 → 1 → 5 → 3 (stale + overflow are simple fixes, do them first to avoid conflicts)