# Realtime Subscriptions Guide

## How It Works

Supabase Realtime uses WebSockets to broadcast database changes to connected clients. When an ESP32 inserts a row into `telemetry_live`, all browsers subscribed to that device receive the new data within milliseconds.

Under the hood, Supabase uses PostgreSQL's `LISTEN/NOTIFY` + the `publication` we defined in the schema:

```sql
create publication supabase_realtime for table public.telemetry_live, public.devices;
```

---

## React — useRealtime Hook

```typescript
// src/hooks/useRealtime.ts
import { useEffect, useState, useRef } from 'react'
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
```

---

## Channel Filter Syntax

Supabase Realtime uses dot-notation filters:

| Filter | Meaning |
|---|---|
| `device_id=eq.device-key` | device_id equals this value |
| `recorded_at=gte.2024-01-01` | recorded_at >= date |
| Multiple filters | comma-separated: `device_id=eq.key,recorded_at=gte.date` |

### Filter Operators

| Operator | Description |
|---|---|
| `eq` | equals |
| `neq` | not equals |
| `gt` | greater than |
| `gte` | greater than or equal |
| `lt` | less than |
| `lte` | less than or equal |
| `like` | SQL LIKE pattern |
| `ilike` | case-insensitive LIKE |

---

## Dashboard Integration

In the Dashboard page:

```typescript
import { useRealtime } from '../hooks/useRealtime'
import { useTelemetry } from '../hooks/useTelemetry'

function Dashboard({ deviceKey }: { deviceKey: string }) {
  // Fetch historical data on mount
  const { historicalData } = useTelemetry(deviceKey)

  // Subscribe to live updates
  const { dataPoints, latestReading } = useRealtime(deviceKey)

  // Merge: show historical + live rolling window
  const chartData = [...historicalData, ...dataPoints]

  return (
    <div>
      <TelemetryChart data={chartData} />
    </div>
  )
}
```

---

## Telemetry Historical Data Hook

```typescript
// src/hooks/useTelemetry.ts
import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'
import type { TelemetryPoint } from '../lib/types'

export function useTelemetry(deviceKey: string | null) {
  const [historicalData, setHistoricalData] = useState<TelemetryPoint[]>([])
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    if (!deviceKey) return

    async function fetchHistory() {
      const { data, error } = await supabase
        .from('telemetry_live')
        .select('*')
        .eq('device_id', deviceKey)
        .order('recorded_at', { ascending: false })
        .limit(200)

      if (!error && data) {
        setHistoricalData(data.reverse()) // ascending for chart
      }
      setLoading(false)
    }

    fetchHistory()
  }, [deviceKey])

  return { historicalData, loading }
}
```

---

## Debugging Realtime

### Check if realtime is enabled

In Supabase dashboard → Database → Replication. You should see `telemetry_live` and `devices` in the publication.

### Test with Supabase dashboard

1. Open Supabase dashboard → Table Editor → telemetry_live
2. Open the React dashboard in another tab, select a device
3. Manually insert a row via SQL Editor:
   ```sql
   insert into public.telemetry_live (device_id, payload, metadata)
   values ('your-device-key', '{"test": 1.23}', '{}');
   ```
4. The chart in the React dashboard should update within 1-2 seconds

### Browser console logging

Add to your realtime callback to debug:
```typescript
console.log('Realtime event:', payload.eventType, payload.new)
```

---

## Performance Notes

- Supabase free tier allows **200 concurrent Realtime connections** per project
- Each subscribed channel counts as one connection
- Channels are automatically cleaned up when the component unmounts (via `removeChannel`)
- For very high-frequency devices (1 sample/second), consider:
  - Aggregating on ESP32 side to 10-second averages before POST
  - Using a 1-second minimum POST interval
  - Throttling chart updates to 1 per second via `requestAnimationFrame`

---

## Connection Status

```typescript
function useRealtimeStatus(deviceKey: string | null) {
  const [status, setStatus] = useState<'connecting' | 'connected' | 'disconnected'>('disconnected')

  useEffect(() => {
    if (!deviceKey) return

    const channel = supabase.channel(`status-${deviceKey}`)

    channel
      .on('system', { event: 'connected' }, () => setStatus('connected'))
      .on('system', { event: 'disconnected' }, () => setStatus('disconnected'))
      .on('system', { event: 'error' }, () => setStatus('disconnected'))
      .subscribe(status => {
        if (status === 'SUBSCRIBED') setStatus('connecting')
      })

    return () => { supabase.removeChannel(channel) }
  }, [deviceKey])

  return status
}
```