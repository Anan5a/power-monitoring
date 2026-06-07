# Dashboard Redesign — Design Spec

**Date:** 2026-06-07
**Status:** Approved
**Author:** brainstorm session

## Problem

The current dashboard (`/home/sayem/sources/power-monitoring/ui/src/pages/DashboardPage.tsx`) has two pathologies that compound:

1. **Memory creep over 24h viewing.** Five child components (`PowerHistoryChart`, `QuickStatsRow`, `DailyGenerationCard`, `BatteryChargeCard`, `RelaySwitchRow`) each open independent Supabase realtime subscriptions and run independent `setInterval` polls. Buffers grow without bound on long ranges (50k-point cap in `PowerHistoryChart`).
2. **Re-rendering artifacts on every refresh.** `framer-motion`'s `AnimatePresence` is used to animate number changes in `QuickStatsRow` (`AnimatedNumber`), `InverterPowerCard`, `BatteryChargeCard`, and `DailyGenerationCard`. Every 5-15s poll re-mounts the animated span, causing visible y-jitter. `recharts` `<ResponsiveContainer>` re-paints the full SVG tree on every prop change. Layout shifts when content height changes (the team has been chasing this with five recent commits).

User-confirmed success criteria (must all be met):
- Heap stays flat over 24h of viewing
- Live chart renders 60fps on 7d/30d ranges
- No visible flicker or layout shift on data refresh
- Works on mid-tier Android (Moto G / Samsung A-series)

## Goals

- Solve all four success criteria
- Keep the team in React + Vite + Tailwind + Supabase (lowest context cost)
- Support a future page-builder (drag/drop widget arrangement) without rewriting the architecture
- Preserve the old dashboard as a fallback at `/dashboard/legacy` for one release

## Non-Goals

- Building the page-builder UI in v1 (drag/drop, widget picker). The architecture supports it; the UI ships later.
- Multi-device fleet view. Architecture supports it; UI ships later.
- Replacing Supabase as the backend.
- Changing the firmware or backend services.

## Stack

| Concern | Choice | Reason |
|---|---|---|
| Build | Vite 5 (unchanged) | Already in use, team knows it |
| Framework | React 18 (unchanged) | Already in use, team knows it |
| Styling | Tailwind 3 (unchanged) | Already in use |
| State | **Jotai** (new) | Atom-family pattern fits future multi-device; derived atoms for computed telemetry; first-class async via `loadable()` |
| Chart | **uPlot** (replaces recharts) | Canvas, 60fps with 10k+ points, ~40KB gzipped |
| Animation | **CSS transitions only** (replaces framer-motion) | GPU-accelerated, no remount churn |
| Backend | Supabase JS (unchanged) | Already in use |

Removed: `framer-motion`, `recharts`.

## Architecture

Three layers. No cross-coupling. Single source of truth for telemetry.

### Layer 1: Atoms (`ui/src/state/`)

Primitive atoms:
- `latestAtom: TelemetryPoint | null` — set by `telemetryService` on every Supabase INSERT
- `connectionStateAtom: 'connecting' | 'live' | 'stale' | 'offline'`
- `nowAtom` — `atomWithReducer(Date.now(), ...)` updated once per second by a single `setInterval` in `App.tsx`
- `deviceChannelsAtomFamily(deviceKey): DeviceChannels | null`

Derived atoms (module-level, recompute lazily):
- `liveBufferAtom` — capped at 200 points via `slice(-199)`
- `computedTelemetryAtom` — `pv_power`, `battery_power`, `inverter_power`, `system_status`, etc. (delegates to `lib/computedTelemetry.ts` only as fallback when server column is null)
- `channelPayloadAtomFamily(channelIdx)` — returns `{ V, I, P, energyWh, socPct, batteryCapacity }`
- `secondsAgoAtom` — derived from `latestAtom.recorded_at` vs `nowAtom`
- `inverterPowerAtom`, `generationTotalAtom(deviceId)`, `batteryChargeAtom(deviceId)` — feed individual widgets

History atoms (using `loadable()`):
- `historyAtomFamily({ deviceKey, range, metric })` — async atom that runs the RPC. Returns `{ loading, data, error }` via `useAtomValue`.
- `historyErrorAtomFamily(...)` — surfaces RPC errors
- `refreshTriggerAtom` — bumped by Refresh button; `historyAtomFamily` re-runs when this changes

Layout atoms:
- `layoutAtom: LayoutDoc` — current widget arrangement `[{ id, type, props, gridArea }, ...]`
- `widgetRegistryAtom: Map<WidgetType, WidgetDef>` — static, built once at app start
- `widgetPropsAtomFamily(widgetId)` — per-widget-instance overrides

### Layer 2: Services (`ui/src/state/services/`)

Side-effect owners. Each one is a module that exports `start*` / `stop*` functions. They write to atoms; they don't expose React state.

- `telemetryService.ts`:
  - `startLiveTelemetry(deviceKey): () => void` — opens Supabase channel `telemetry-${deviceKey}` filtered to `telemetry_computed` INSERTs. Calls `setLatestAtom(point)`. Owns the 15s stale timer. Pauses timer on `document.visibilitychange === 'hidden'`. Returns cleanup function.
  - `stopLiveTelemetry()` — removes channel, clears timer.

- `historyService.ts`:
  - `loadHistory(deviceKey, range, metric)` — wraps RPC. Reads/writes `historyAtomFamily`. Cache scoped to `(deviceKey, range, metric)`; switch device/range/metric = new atom instance, automatic.

- `channelsService.ts`:
  - `loadChannels(deviceKey)` — wraps `fetchDeviceChannels` from `lib/supabase.ts`. Writes to `deviceChannelsAtomFamily(deviceKey)`. Cached for session.

- `relayService.ts`:
  - `loadRelays(deviceKey)`, `toggleRelay(relayState)`, `subscribeRelays(deviceKey)` — separate from telemetry. Returns cleanup.

- `layoutService.ts`:
  - `loadLayout(userId)`, `saveLayout(userId, doc)` — reads/writes Supabase `user_dashboard_layouts` table. First-time user gets default `LayoutDoc`.

### Layer 3: UI (`ui/src/widgets/` + `ui/src/pages/`)

Pure presentational. Subscribe via `useAtomValue`. Memoized with `React.memo`. Animate with CSS only.

- `WidgetHost` — looks up `widgetRegistryAtom` for the type, renders the widget with its props
- `WidgetGrid` — CSS Grid renderer. Reads `layoutAtom`. Maps each entry to `<WidgetHost />`. Fixed row heights. No flex grow.
- Widget components (one file each):
  - `QuickStatsWidget.tsx` — replaces `QuickStatsRow.tsx`
  - `RelaysWidget.tsx` — replaces `RelaySwitchRow.tsx`
  - `InverterWidget.tsx` — replaces `InverterPowerCard.tsx`
  - `GenerationWidget.tsx` — replaces `DailyGenerationCard.tsx`
  - `BatteryWidget.tsx` — replaces `BatteryChargeCard.tsx`
  - `VCCardWidget.tsx` — parameterized by `channel` prop; replaces `VCDashboardCard.tsx`
  - `HistoryChartWidget.tsx` — replaces `PowerHistoryChart.tsx`. Lazy-loads `uplot` via `React.lazy`.
  - `SpacerWidget.tsx` — utility for layout
  - `PlaceholderWidget.tsx` — utility for layout

### Data flow

```
Supabase INSERT (telemetry_computed)
  → telemetryService.handleMessage()
    → setLatestAtom(point)
      → derived atoms recompute lazily (only changed ones)
        → subscribed widgets re-render
      → uPlot.setData() pushes new point into chart (no React render)
```

One subscription per page. One buffer. One render path per changed atom.

## Layout

CSS Grid, 12 columns on desktop, 6 on tablet, 2 on mobile. `grid-auto-rows: 80px`. Widgets declare `gridArea: { col, row, colSpan, rowSpan }`. No flex grow.

### Default LayoutDoc (v1)

| Row | Widget | Cols | Rows |
|---|---|---|---|
| 1 | HeaderBar | 12 | 1 |
| 2-3 | QuickStats | 12 | 2 |
| 4 | Relays | 12 | 1 |
| 5-6 | Inverter / Generation / Battery / Spacer | 3 each | 2 |
| 7-8 | VCCard × 4 | 3 each | 2 |
| 9-14 | HistoryChart | 12 | 6 |

### Anti-shift rules

- Every widget has a fixed `min-height` matching its largest expected content.
- Loading skeletons reserve the same space as content (no flash).
- Numbers live in fixed-size boxes with `font-variant-numeric: tabular-nums; text-align: right`.
- No `vh` units for critical dimensions. `px` or `rem` only.
- `overflow: hidden` on body. No `scroll-anchoring` (causes jumps).
- Layout saved to Supabase per user; first visit gets the default.

### Sidebar

Stays as-is. Add one entry: "Dashboard (Legacy)" → `/dashboard/legacy`.

### HeaderBar

- Sticky. Single `setInterval` updates `nowAtom` at 1Hz.
- "X seconds ago" derived from `secondsAgoAtom`.
- Stale badge driven by `connectionStateAtom`.

## Widget details

### QuickStatsWidget

- Subscribes to: `latestAtom`, `computedTelemetryAtom`, `deviceChannelsAtomFamily(deviceKey)`
- Renders: Total Power / PV / Inverter / Battery / per-battery SoC bars / status pill
- Numbers: static `<span>` with `tabular-nums`. No `AnimatePresence`. CSS `transition: color 200ms` for state-class changes (charging/balanced/discharging).
- SoC bar: CSS `transition: width 300ms ease-out`.
- `overflow-x-auto overflow-y-hidden` reserved.
- Fixed height, content centered.

### HistoryChartWidget

- Subscribes to: `historyAtomFamily({ deviceKey, range, metric })` for historical range, `latestAtom` for live append.
- **uPlot replaces recharts.**
- Live data: a `useRef` holds the uPlot series array. `useEffect` watches `latestAtom`, calls `uplot.setData([xValues, yValues])`. **No React render on live tick.**
- Range/metric change: dispatched via `historyService`. Loading state shows skeleton, not spinner.
- Downsample: 1500-point cap in `historyService`, not in component. The chart-internal `extractKeys` logic (which decides which series to draw based on metric + non-zero data) moves from `PowerHistoryChart.tsx` into `historyService.ts` so the series-list is computed once per fetch, not per render.
- 30d range: server already aggregates to ~720 hourly buckets via `get_aggregated_telemetry`; no client work.
- Custom tooltip: separate React component, reads `hoveredPointAtom` which uPlot's `setCursor` hook updates. Re-renders only on hover change.
- Lazy-loaded: `React.lazy(() => import('uplot'))` so the 40KB doesn't bloat initial bundle.
- Replaces the 672-line `PowerHistoryChart.tsx` with a focused ~300-line file.

### Chart interaction: zoom + drilldown

The chart supports two interaction modes for inspecting data at finer resolution.

**Drag-to-zoom (uPlot native)**
- uPlot's `cursor: { drag: { x: true, y: false } }` enables horizontal drag selection out of the box. While dragging, the chart dims the un-selected region with a translucent overlay.
- On drag-end: capture the selected time range `[tStart, tEnd]` from the cursor's `select` event. Set `zoomRangeAtom = { start: tStart, end: tEnd }`.
- Double-click on the chart: clear `zoomRangeAtom`, return to the active range button's view.
- Zoom is purely client-side within the loaded data (no re-fetch). The user sees the resolution already in the buffer, just magnified.

**Click-to-drilldown (summary buckets)**
- When the active range is `24h` / `7d` / `30d`, each x-axis bucket (hour, day) is clickable.
- On click: capture the bucket's time range, dispatch `historyService.loadHistory(deviceKey, drilldownRange, metric)` where `drilldownRange` is computed:
  - From `7d` clicking a day → load `1h` slice covering that day
  - From `30d` clicking a day → load `6h` slice covering that day
  - From `24h` clicking an hour → load `1h` slice covering that hour (already the minimum)
- While the drilldown loads: chart shows a skeleton overlay with the same dimensions (no layout shift).
- A breadcrumb appears above the chart: `7d → 2026-06-05`. Each segment is clickable to jump back.

**State**
- `zoomRangeAtom: { start: number; end: number } | null` — drag-zoom window
- `drilldownBreadcrumbAtom: Array<{ range: Range, label: string, tStart: number, tEnd: number }>` — drilldown stack
- `historyService.loadHistory` reads both: if drilldown active, query `(tStart, tEnd)` window; if zoom active, use the existing buffer.

**Data fetch on drilldown**
- New atoms: `drilldownLoadableAtom({ deviceKey, tStart, tEnd, metric })` — loadable async atom, same RPC path as `historyAtomFamily` but with explicit time window instead of preset range.
- The typed-columns query is used for any window ≤ 6h (full resolution, ≤ 2160 rows). Longer windows fall back to the aggregated RPC.
- 1500-point cap still applies inside the fetched window.

**UI**
- Range buttons (1h/6h/24h/7d/30d) remain. Drilldown doesn't disable them — clicking a range button clears the breadcrumb and zoom.
- "Reset zoom" button appears when `zoomRangeAtom` is set.
- Breadcrumb sits between the range buttons and the chart, right-aligned. Replaces the existing chart header layout.

**Performance**
- uPlot's drag-zoom is GPU-light (canvas + DOM overlay). No React re-render on drag.
- `drilldownLoadableAtom` is `loadable()` so the chart shows a skeleton, not a blank state.
- uPlot's `setScale` is called directly from the drag-end handler via a ref — no React render to apply the zoom.

### Widget registry (v1)

```ts
type WidgetDef = {
  type: string
  label: string
  component: React.ComponentType<any>
  defaultProps?: Record<string, unknown>
  defaultSize: { col: number; row: number; colSpan: number; rowSpan: number }
}

const registry: Record<string, WidgetDef> = {
  quickstats: { ... },
  relays: { ... },
  inverter: { ... },
  generation: { ... },
  battery: { ... },
  vc0: { component: VCCardWidget, defaultProps: { channel: 0 }, ... },
  vc1: { component: VCCardWidget, defaultProps: { channel: 1 }, ... },
  vc2: { component: VCCardWidget, defaultProps: { channel: 2 }, ... },
  vc3: { component: VCCardWidget, defaultProps: { channel: 3 }, ... },
  history: { ... },
  spacer: { ... },
  placeholder: { ... },
}
```

Adding a new widget = register + add to default `LayoutDoc`. No grid code changes.

## Data layer details

### Supabase channels

Two channels, not one. Reasons:
- Different filters per table; mixing is awkward
- Different lifecycles (telemetry has stale-timer + visibility-pause; relays don't)
- Error isolation: a relay-table RLS failure shouldn't kill telemetry

Cost: 2 websocket subscriptions. Trivial.

### Why `computeTelemetry()` on the client becomes mostly redundant

`useComputedTelemetry.ts` is deleted. Server-computed columns in `telemetry_computed` already include `pv_power`, `battery_power`, `inverter_power`, `system_status`, `min_soc_pct`, `max_soc_pct`, `total_energy_wh`. The client `lib/computedTelemetry.ts` is kept only as a fallback for older rows where the server column is null. Widgets prefer server values; they fall back to the client function only if the server value is missing.

This eliminates the duplicate computation that ran per row at 5Hz on the client.

### `loadable()` for history atoms

```ts
import { atom } from 'jotai'
import { loadable } from 'jotai/utils'

const historyAtomFamily = atomFamily((params) =>
  loadable(atom(async (get) => {
    get(refreshTriggerAtom)
    const { data, error } = await supabase.rpc('get_aggregated_telemetry', {...})
    if (error) throw error
    return data
  }))
)
```

`useAtomValue` returns `{ loading, data, error }`. Widgets render the appropriate state without `useEffect`.

### Visibility-pause (fixes 24h heap creep)

`document.addEventListener('visibilitychange', ...)` in `telemetryService`:
- Hidden: stop the stale timer; supabase channel stays open (cheap, WebSocket)
- Visible: resume timer, force `connectionStateAtom = 'connecting'` briefly

The 200-point live buffer cap handles any growth while hidden.

### `nowAtom`

```ts
const nowAtom = atomWithReducer(Date.now(), () => Date.now())
// Single setInterval in App.tsx: useUpdateAtom(nowAtom) at 1Hz
```

Widgets that need "X seconds ago" derive it. Widgets that don't care don't subscribe. Fixes the 1Hz re-render storm the old `HeaderBar` had.

## Migration

1. Add `jotai` and `uplot` to `package.json`.
2. Build new state layer in `ui/src/state/` — doesn't touch existing code.
3. Build new widget components in `ui/src/widgets/` — alongside existing `ui/src/components/`.
4. Build new `WidgetGrid` and updated `DashboardLayout`.
5. Move old `DashboardPage` to `pages/legacy/DashboardPage.tsx`. Add `/dashboard/legacy` route.
6. Update `Sidebar` to add "Dashboard (Legacy)" entry.
7. Cutover: flip default `/dashboard` route to new tree. Keep legacy accessible.
8. Delete replaced files: `useRealtime.ts`, `useComputedTelemetry.ts`, `useDailyGeneration.ts`, `useBatteryCharge.ts`, `PowerHistoryChart.tsx`, `QuickStatsRow.tsx`, `DailyGenerationCard.tsx`, `InverterPowerCard.tsx`, `BatteryChargeCard.tsx`, `VCDashboardCard.tsx`, `RelaySwitchRow.tsx`.
9. Remove `framer-motion` and `recharts` from `package.json`. Re-run install.
10. Commit-by-commit for review.

## Testing

### Unit (Vitest)

- Atoms: derived atom selectors with synthetic state
- `extractKeys` logic (moved to `historyService` from the old `PowerHistoryChart.tsx`)
- `computeTelemetry` (kept as fallback for older rows)
- `keyToLabel` formatter

### Integration (Playwright)

- **Smoke:** `/dashboard` renders, no console errors, Supabase channel subscribes
- **Long-running:** dashboard open 5 min, no heap growth > 5MB, no console warnings
- **Range switching:** 1h/6h/24h/7d/30d all load and display
- **Realtime:** new sample appears within 1s of INSERT
- **Relays:** toggle works
- **Stale:** indicator appears after 15s of no updates
- **Visibility:** pause/resume works

### Performance budget

Lighthouse + Chrome DevTools Performance tab. Targets:
- TTI < 2s on Moto G Power profile (4x CPU throttle, Slow 4G)
- LCP < 2.5s
- Heap delta over 1h session < 10MB
- Chart FPS > 55fps during live updates on 7d range
- Cumulative Layout Shift = 0

### Visual regression

Chromatic or Playwright screenshot diff on each widget.

### Manual checklist (before cutover)

- [ ] All device types render correctly
- [ ] Realtime updates appear within 1s
- [ ] Range switches load and display
- [ ] Relay toggles work
- [ ] Battery charge updates
- [ ] Generation card shows hourly + daily data
- [ ] Stale indicator appears after 15s of no updates
- [ ] Tab visibility pause/resume works
- [ ] Chart drag-zoom works (drag horizontally, see magnified range)
- [ ] Chart double-click resets zoom
- [ ] Chart click-to-drill works on summary buckets (7d day → 1h slice, 30d day → 6h slice, 24h hour → 1h slice)
- [ ] Drilldown breadcrumb shows and is clickable
- [ ] No memory growth over 1h session (DevTools Memory profiler)
- [ ] No layout shift on data refresh (DevTools Performance → Layout Shifts)
- [ ] Chart 60fps during live updates (DevTools Performance → FPS meter)
- [ ] Works on mobile viewport (Chrome DevTools device emulation, Moto G Power)
- [ ] Legacy dashboard still works at `/dashboard/legacy`

### Rollback

If new dashboard has a critical bug post-cutover: revert the cutover commit. Old code is untouched, the route still exists, users can switch to `/dashboard/legacy`. Bug is contained.

## Risks

- **uPlot learning curve.** uPlot's API is imperative (refs, manual series updates). If the team isn't familiar, expect a day of friction. Mitigation: ship the chart behind `React.lazy`, write a small wrapper that exposes the React-style props the rest of the app already uses.
- **Jotai's `loadable()` may not handle aborted fetches cleanly.** If a user changes range mid-fetch, the old promise resolves and may set stale data. Mitigation: scope cache by `(deviceKey, range, metric)` and check the atom's identity before applying.
- **LayoutDoc persistence is a new surface.** If we serialize widget props naively, future widget versions may break old layouts. Mitigation: version the schema; ignore unknown fields on load.
- **`uplot` types are weak.** The package has `@types/uplot` but coverage is incomplete. Mitigation: a thin typed wrapper around the parts we use.

## Open questions

None at this time. Spec is approved.
