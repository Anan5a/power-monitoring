# Daily PV Generation Card — Design

## Goal

Show cumulative solar energy generated today (kWh) with an hourly sparkline breakdown, displayed as a card beside the InverterPowerCard on the dashboard.

## Data Source

- **Table:** `telemetry_computed`
- **Time range:** today 00:00 to now
- **Metric:** sum of PV power from channels tagged as solar in `channel_groups` (where `icon = 0`)
- **Bucket:** hourly (each bucket = sum of power readings in that hour, converted to Wh)
- **Aggregation:** `sum(pv_power * interval_length) / 3600` per hour — or simpler: `avg(pv_power) * hours_in_bucket` where the bucket is ~1 hour

## UI Component: `DailyGenerationCard`

**File:** `ui/src/components/DailyGenerationCard.tsx`

**Props:**
```typescript
interface Props {
  deviceKey: string
  deviceChannels?: DeviceChannels | null
}
```

**Layout:**
- Card with gradient header (amber/yellow tone — solar theme)
- Large kWh display (2 decimal places, e.g. "12.34 kWh")
- Label: "Today's Generation"
- Sparkline: small inline area chart showing hourly generation (last 24 hours of today), 80px tall
- Time range label below sparkline: "6am → 8pm" or "00:00 → now"

**States:**
- Loading: pulsing skeleton
- No data: "-- kWh" with "No generation data today"
- Active: animated counter + live sparkline

## Hook: `useDailyGeneration`

**File:** `ui/src/hooks/useDailyGeneration.ts`

```typescript
export function useDailyGeneration(deviceKey: string | null, channelGroups: ChannelGroup[] | undefined): {
  total: number       // kWh today
  hourly: Array<{ hour: string; value: number }>  // value in kWh
  isLoading: boolean
}
```

**Behavior:**
1. On mount and deviceKey change, fetch today data
2. Filter `channelGroups` for solar (`icon === 0`), extract channel indices
3. Build hourly buckets from 00:00 to current hour
4. Subscribe to new inserts on `telemetry_computed` for live updates
5. Return `{ total, hourly, isLoading }`

**Query strategy:**
```sql
-- Get today's PV power per hour
select
  date_trunc('hour', recorded_at) as hour,
  avg(pv_power) as avg_power_w
from telemetry_computed
where device_key = $1
  and recorded_at >= current_date
  and recorded_at < current_timestamp
group by date_trunc('hour', recorded_at)
order by hour
```

Each hourly bucket contributes `avg_power_w * (bucket_duration_sec / 3600) / 1000` kWh.

## Dashboard Integration

**File:** `ui/src/pages/DashboardPage.tsx`

Add `DailyGenerationCard` to the top metrics grid beside `InverterPowerCard`:

```tsx
<div className="grid grid-cols-1 md:grid-cols-2 gap-4">
  <DailyGenerationCard deviceKey={selectedDevice.device_key} deviceChannels={deviceChannels} />
  <InverterPowerCard inverterPower={computed.inverter_power} systemStatus={computed.system_status} />
  <RelaySwitchRow deviceKey={selectedDevice.device_key} />
</div>
```

## Sparkline

Use a minimal inline SVG or the existing chart library. No axes, no labels on the line — just the gradient area + a hover tooltip showing hour and kWh.

## Error Handling

- If query fails, show "-- kWh" with no sparkline
- If no solar channels configured, show "No solar channels configured"
- If deviceKey is null, show empty/placeholder state
