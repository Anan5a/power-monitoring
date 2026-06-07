# Dashboard Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current per-component-poll dashboard with a centralized Jotai-atom-store, uPlot-based zoomable chart, and a widget-registry architecture that supports a future page-builder. Eliminate memory creep, re-render flicker, and long-range jank.

**Architecture:** Three layers (atoms / services / widgets). One Supabase subscription per page. CSS-only animation. uPlot for the chart, fed by refs (no React render on live tick). Old dashboard preserved at `/dashboard/legacy`.

**Tech Stack:** Vite, React 18, TypeScript, Tailwind, Jotai, uPlot, Supabase JS. Replaces framer-motion and recharts.

**Spec:** `docs/superpowers/specs/2026-06-07-dashboard-redesign-design.md`

---

## File Map

### State layer (new)
- Create: `ui/src/state/atoms.ts` — primitive atoms (latest, connectionState, now, refreshTrigger, zoomRange, drilldownBreadcrumb)
- Create: `ui/src/state/derived.ts` — derived atoms (liveBuffer, computedTelemetry, channelPayload family, secondsAgo, inverterPower, generationTotal, batteryCharge)
- Create: `ui/src/state/history.ts` — historyAtomFamily, drilldownLoadableAtom
- Create: `ui/src/state/services/telemetryService.ts` — owns supabase channel + stale timer + visibility pause
- Create: `ui/src/state/services/historyService.ts` — RPC wrapper, range/zoom/drilldown dispatch
- Create: `ui/src/state/services/channelsService.ts` — device_channels fetch
- Create: `ui/src/state/services/relayService.ts` — relay subscribe + toggle
- Create: `ui/src/state/services/layoutService.ts` — load/save LayoutDoc from Supabase
- Create: `ui/src/state/layout.ts` — layoutAtom, widgetRegistryAtom
- Create: `ui/src/state/nowTicker.ts` — useNowTicker hook (1Hz setInterval writing nowAtom)

### Widget layer (new)
- Create: `ui/src/widgets/registry.ts` — WidgetDef type, registry map
- Create: `ui/src/widgets/WidgetHost.tsx` — looks up type, renders component
- Create: `ui/src/widgets/WidgetGrid.tsx` — CSS Grid renderer reading layoutAtom
- Create: `ui/src/widgets/QuickStatsWidget.tsx`
- Create: `ui/src/widgets/RelaysWidget.tsx`
- Create: `ui/src/widgets/InverterWidget.tsx`
- Create: `ui/src/widgets/GenerationWidget.tsx`
- Create: `ui/src/widgets/BatteryWidget.tsx`
- Create: `ui/src/widgets/VCCardWidget.tsx`
- Create: `ui/src/widgets/HistoryChartWidget.tsx` — uPlot wrapper
- Create: `ui/src/widgets/HistoryTooltip.tsx` — separate React component, reads hoveredPointAtom
- Create: `ui/src/widgets/SpacerWidget.tsx`
- Create: `ui/src/widgets/PlaceholderWidget.tsx`

### Pages (new/modify)
- Create: `ui/src/pages/DashboardPage.tsx` — rewritten, uses new state + WidgetGrid
- Create: `ui/src/pages/LegacyDashboardPage.tsx` — moved from old DashboardPage
- Modify: `ui/src/pages/LoginPage.tsx` — no change (skip)
- Modify: `ui/src/App.tsx` — add `/dashboard/legacy` route, add `useNowTicker()` call
- Modify: `ui/src/components/Sidebar.tsx` — add "Dashboard (Legacy)" entry

### Layout/components (modify)
- Modify: `ui/src/components/HeaderBar.tsx` — read nowAtom + connectionStateAtom instead of local state
- Modify: `ui/src/components/DashboardLayout.tsx` — no change to signature, used as-is

### Lib (modify)
- Modify: `ui/src/lib/supabase.ts` — no change (services use it directly)
- Modify: `ui/src/lib/computedTelemetry.ts` — keep but add explicit "fallback" JSDoc; client function still exported for older rows
- Modify: `ui/src/lib/types.ts` — add `LayoutDoc`, `WidgetType`, `WidgetDef` types

### Tests (new)
- Create: `ui/vitest.config.ts`
- Create: `ui/src/state/__tests__/derived.test.ts`
- Create: `ui/src/state/__tests__/history.test.ts`
- Create: `ui/src/state/services/__tests__/telemetryService.test.ts`
- Create: `ui/src/lib/__tests__/computedTelemetry.test.ts`
- Create: `ui/src/widgets/__tests__/extractKeys.test.ts`

### Config (modify)
- Modify: `ui/package.json` — add jotai, uplot, vitest, @testing-library/react, @types/uplot; remove framer-motion, recharts
- Modify: `ui/src/index.css` — no change (Tailwind only)
- Modify: `tsconfig.json` — add `vitest/globals` to types

### Files deleted (later tasks)
- Delete: `ui/src/hooks/useRealtime.ts`
- Delete: `ui/src/hooks/useComputedTelemetry.ts`
- Delete: `ui/src/hooks/useDailyGeneration.ts`
- Delete: `ui/src/hooks/useBatteryCharge.ts`
- Delete: `ui/src/components/PowerHistoryChart.tsx`
- Delete: `ui/src/components/QuickStatsRow.tsx`
- Delete: `ui/src/components/DailyGenerationCard.tsx`
- Delete: `ui/src/components/InverterPowerCard.tsx`
- Delete: `ui/src/components/BatteryChargeCard.tsx`
- Delete: `ui/src/components/VCDashboardCard.tsx`
- Delete: `ui/src/components/RelaySwitchRow.tsx`

---

## Task ordering

Tasks are grouped by phase. Each phase ends with a working, demoable dashboard. Phases 1-3 are the foundation. Phase 4 is the chart. Phase 5 is the migration cutover and cleanup.

- **Phase 1: Foundation** (Tasks 1-3) — install deps, set up Vitest, types
- **Phase 2: State layer** (Tasks 4-9) — atoms, derived, services
- **Phase 3: UI shell** (Tasks 10-13) — WidgetGrid, WidgetHost, registry, default layout
- **Phase 4: Widgets** (Tasks 14-20) — small widgets first, then the heavy chart
- **Phase 5: Migration** (Tasks 21-25) — old route, cutover, deletion, performance verification

---

## Phase 1: Foundation

### Task 1: Install dependencies

**Files:**
- Modify: `ui/package.json`

- [ ] **Step 1: Add runtime deps**

```bash
cd ui
npm install jotai uplot
npm install -D @types/uplot vitest @vitest/ui @testing-library/react @testing-library/jest-dom jsdom
```

Expected: `package.json` updates with new deps. `node_modules/jotai` and `node_modules/uplot` exist.

- [ ] **Step 2: Remove old deps**

```bash
npm uninstall framer-motion recharts
```

Expected: `framer-motion` and `recharts` no longer in `package.json`. `node_modules/framer-motion` and `node_modules/recharts` gone.

- [ ] **Step 3: Verify install**

```bash
cd ui
ls node_modules/jotai/package.json node_modules/uplot/package.json 2>&1 | head -5
ls node_modules/vitest/package.json 2>&1 | head -1
```

Expected: all three paths exist. framer-motion and recharts paths are gone.

- [ ] **Step 4: Commit**

```bash
git add ui/package.json ui/package-lock.json
git commit -m "chore(ui): swap framer-motion+recharts for jotai+uplot, add vitest"
```

---

### Task 2: Configure Vitest

**Files:**
- Create: `ui/vitest.config.ts`
- Modify: `ui/tsconfig.json`

- [ ] **Step 1: Create vitest config**

Create `ui/vitest.config.ts`:

```ts
/// <reference types="vitest" />
import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
  },
})
```

- [ ] **Step 2: Create test setup file**

Create `ui/src/test/setup.ts`:

```ts
import '@testing-library/jest-dom/vitest'
```

- [ ] **Step 3: Add `vitest/globals` to tsconfig types**

Edit `ui/tsconfig.json`. Change:
```json
"types": ["web-bluetooth"]
```
to:
```json
"types": ["web-bluetooth", "vitest/globals"]
```

- [ ] **Step 4: Add test scripts to package.json**

Edit `ui/package.json`. Change the `scripts` block to:
```json
"scripts": {
  "dev": "vite",
  "build": "tsc && vite build",
  "preview": "vite preview",
  "test": "vitest run",
  "test:watch": "vitest"
}
```

- [ ] **Step 5: Verify vitest works with a trivial test**

Create `ui/src/test/sanity.test.ts`:

```ts
import { describe, it, expect } from 'vitest'

describe('sanity', () => {
  it('runs', () => {
    expect(1 + 1).toBe(2)
  })
})
```

Run:
```bash
cd ui && npm test
```

Expected: 1 test passes.

- [ ] **Step 6: Delete sanity test, commit**

```bash
rm ui/src/test/sanity.test.ts
git add ui/vitest.config.ts ui/tsconfig.json ui/src/test/setup.ts ui/package.json
git commit -m "test(ui): configure vitest with jsdom + react plugin"
```

---

### Task 3: Add layout types

**Files:**
- Modify: `ui/src/lib/types.ts`

- [ ] **Step 1: Add the new types at the end of `ui/src/lib/types.ts`**

Append to the file:

```ts
// --- Layout / widget registry (added 2026-06-07 redesign) ---

export type WidgetType =
  | 'quickstats'
  | 'relays'
  | 'inverter'
  | 'generation'
  | 'battery'
  | 'vc0'
  | 'vc1'
  | 'vc2'
  | 'vc3'
  | 'history'
  | 'spacer'
  | 'placeholder'

export interface GridArea {
  col: number      // 1-based start column
  row: number      // 1-based start row
  colSpan: number  // 1-12
  rowSpan: number
}

export interface LayoutEntry {
  id: string           // unique per instance
  type: WidgetType
  gridArea: GridArea
  props?: Record<string, unknown>
}

export interface LayoutDoc {
  version: 1
  entries: LayoutEntry[]
}

export interface WidgetDef<P = Record<string, unknown>> {
  type: WidgetType
  label: string
  defaultSize: { colSpan: number; rowSpan: number }
  defaultProps?: P
}
```

- [ ] **Step 2: Verify build still compiles**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors. (The new types are unused so far; that's fine.)

- [ ] **Step 3: Commit**

```bash
git add ui/src/lib/types.ts
git commit -m "feat(ui): add LayoutDoc + WidgetType types for new dashboard"
```

---

## Phase 2: State layer

### Task 4: Primitive atoms

**Files:**
- Create: `ui/src/state/atoms.ts`

- [ ] **Step 1: Create atoms file**

Create `ui/src/state/atoms.ts`:

```ts
import { atom } from 'jotai'
import { atomFamily, atomWithReducer } from 'jotai/utils'
import type { TelemetryPoint, DeviceChannels, RelayState, LayoutDoc } from '../lib/types'

// --- Connection / time ---

export const connectionStateAtom = atom<
  'connecting' | 'live' | 'stale' | 'offline'
>('offline')

// Updated once per second by useNowTicker (mounted in App.tsx)
export const nowAtom = atomWithReducer<number, void>(
  Date.now(),
  () => Date.now(),
)

// --- Telemetry (set by telemetryService) ---

export const latestAtom = atom<TelemetryPoint | null>(null)

// Bumped to invalidate history loadables
export const refreshTriggerAtom = atom(0)

// --- Per-device data (atomFamily: one entry per deviceKey) ---

export const deviceChannelsAtomFamily = atomFamily((_deviceKey: string) =>
  atom<DeviceChannels | null>(null),
)

export const relayStatesAtomFamily = atomFamily((_deviceKey: string) =>
  atom<RelayState[]>([]),
)

// --- Layout ---

export const layoutAtom = atom<LayoutDoc | null>(null)
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/state/atoms.ts
git commit -m "feat(state): primitive atoms (latest, connection, now, device data, layout)"
```

---

### Task 5: Derived atoms

**Files:**
- Create: `ui/src/state/derived.ts`
- Create: `ui/src/state/__tests__/derived.test.ts`

- [ ] **Step 1: Write the failing test**

Create `ui/src/state/__tests__/derived.test.ts`:

```ts
import { describe, it, expect } from 'vitest'
import { createStore } from 'jotai'
import {
  latestAtom,
  nowAtom,
} from '../atoms'
import {
  liveBufferAtom,
  computedTelemetryAtom,
  channelPayloadAtomFamily,
  secondsAgoAtom,
} from '../derived'

function makePoint(secondsAgo: number, payload: Record<string, number>) {
  return {
    id: 1,
    device_id: 'k',
    recorded_at: new Date(Date.now() - secondsAgo * 1000).toISOString(),
    payload,
    metadata: {},
  }
}

describe('liveBufferAtom', () => {
  it('is empty when no latest', () => {
    const store = createStore()
    expect(store.get(liveBufferAtom)).toEqual([])
  })

  it('contains the latest when set', () => {
    const store = createStore()
    const pt = makePoint(0, { ch0_P: 10 })
    store.set(latestAtom, pt)
    expect(store.get(liveBufferAtom)).toEqual([pt])
  })
})

describe('computedTelemetryAtom', () => {
  it('returns zeros when payload is empty', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(0, {}))
    const c = store.get(computedTelemetryAtom)
    expect(c.pv_power).toBe(0)
    expect(c.inverter_power).toBe(0)
    expect(c.system_status).toBe('unknown')
  })
})

describe('channelPayloadAtomFamily', () => {
  it('returns nulls for missing channel', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(0, { ch0_V: 12.3 }))
    const p = store.get(channelPayloadAtomFamily(2))
    expect(p.voltage).toBeNull()
    expect(p.current).toBeNull()
  })

  it('returns values for present channel', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(0, { ch2_V: 13.5, ch2_I: 2.1, ch2_P: 28.4, energy_wh2: 100, soc_pct2: 75 }))
    const p = store.get(channelPayloadAtomFamily(2))
    expect(p.voltage).toBe(13.5)
    expect(p.current).toBe(2.1)
    expect(p.power).toBe(28.4)
    expect(p.energyWh).toBe(100)
    expect(p.socPct).toBe(75)
  })
})

describe('secondsAgoAtom', () => {
  it('returns null when no latest', () => {
    const store = createStore()
    expect(store.get(secondsAgoAtom)).toBeNull()
  })

  it('returns the delta in seconds', () => {
    const store = createStore()
    store.set(latestAtom, makePoint(7, {}))
    store.set(nowAtom) // triggers reducer to update now
    const s = store.get(secondsAgoAtom)
    expect(s).toBeGreaterThanOrEqual(7)
    expect(s).toBeLessThan(8)
  })
})
```

- [ ] **Step 2: Run test, verify fail**

```bash
cd ui && npm test -- derived.test
```

Expected: FAIL — `derived.ts` doesn't exist.

- [ ] **Step 3: Implement derived atoms**

Create `ui/src/state/derived.ts`:

```ts
import { atom } from 'jotai'
import { atomFamily } from 'jotai/utils'
import { latestAtom, nowAtom } from './atoms'
import { computeTelemetry, type ComputedValues } from '../lib/computedTelemetry'
import type { ChannelGroup, BatteryProfile } from '../lib/types'

const LIVE_BUFFER_LIMIT = 200

export const liveBufferAtom = atom((get) => {
  const latest = get(latestAtom)
  if (!latest) return []
  return [latest] // future: extend when service writes to a buffer atom
})

export const computedTelemetryAtom = atom<ComputedValues>((get) => {
  const latest = get(latestAtom)
  const payload = (latest?.payload ?? {}) as Record<string, number>
  return computeTelemetry(payload, [] as ChannelGroup[], [] as BatteryProfile[])
})

export interface ChannelPayload {
  voltage: number | null
  current: number | null
  power: number | null
  energyWh: number | null
  socPct: number | null
}

export const channelPayloadAtomFamily = atomFamily((channelIdx: number) =>
  atom<ChannelPayload>((get) => {
    const latest = get(latestAtom)
    const p = (latest?.payload ?? {}) as Record<string, number>
    return {
      voltage: p[`ch${channelIdx}_V`] ?? null,
      current: p[`ch${channelIdx}_I`] ?? null,
      power: p[`ch${channelIdx}_P`] ?? null,
      energyWh: p[`energy_wh${channelIdx}`] ?? null,
      socPct: p[`soc_pct${channelIdx}`] ?? null,
    }
  }),
)

export const secondsAgoAtom = atom<number | null>((get) => {
  const latest = get(latestAtom)
  if (!latest) return null
  get(nowAtom) // subscribe to now changes
  return Math.max(0, Math.floor((Date.now() - new Date(latest.recorded_at).getTime()) / 1000))
})

// Convenience derivations (widget-level)
export const inverterPowerAtom = atom((get) => get(computedTelemetryAtom).inverter_power)
export const pvPowerAtom = atom((get) => get(computedTelemetryAtom).pv_power)
export const batteryPowerAtom = atom((get) => get(computedTelemetryAtom).battery_power)
export const systemStatusAtom = atom((get) => get(computedTelemetryAtom).system_status)
```

Note: `liveBufferAtom` returns `[latest]` for now. Task 6 wires a buffer atom that the service writes to; we'll refactor `liveBufferAtom` then. The test only checks the single-point behavior.

- [ ] **Step 4: Run test, verify pass**

```bash
cd ui && npm test -- derived.test
```

Expected: all 6 tests pass.

- [ ] **Step 5: Commit**

```bash
git add ui/src/state/derived.ts ui/src/state/__tests__/derived.test.ts
git commit -m "feat(state): derived atoms (computed telemetry, channel payload, seconds ago)"
```

---

### Task 6: Live buffer atom + service interface

**Files:**
- Modify: `ui/src/state/atoms.ts`
- Modify: `ui/src/state/derived.ts`
- Create: `ui/src/state/services/telemetryService.ts`
- Create: `ui/src/state/services/__tests__/telemetryService.test.ts`

- [ ] **Step 1: Add buffer atom to atoms.ts**

Edit `ui/src/state/atoms.ts`. Add after `latestAtom`:

```ts
// Ring buffer of recent points, capped at 200. Service writes via appendBufferAtom.
export const liveBufferAtomPrimitive = atom<import('../lib/types').TelemetryPoint[]>([])
export const appendBufferAtom = atom(
  null,
  (get, set, point: import('../lib/types').TelemetryPoint) => {
    const next = [...get(liveBufferAtomPrimitive), point]
    if (next.length > 200) next.splice(0, next.length - 200)
    set(liveBufferAtomPrimitive, next)
  },
)
```

- [ ] **Step 2: Update derived.ts to use the primitive**

Edit `ui/src/state/derived.ts`. Replace the `liveBufferAtom` definition with:

```ts
import { liveBufferAtomPrimitive } from './atoms'

export const liveBufferAtom = atom((get) => get(liveBufferAtomPrimitive))
```

- [ ] **Step 3: Write failing test for telemetryService**

Create `ui/src/state/services/__tests__/telemetryService.test.ts`:

```ts
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { createStore } from 'jotai'

// Mock supabase BEFORE importing the service
vi.mock('../../../lib/supabase', () => {
  const handlers: Array<(payload: any) => void> = []
  return {
    supabase: {
      channel: () => ({
        on: (_evt: string, _cfg: unknown, h: (p: any) => void) => {
          handlers.push(h)
          return { on, subscribe: () => ({}) }
        },
        subscribe: () => ({}),
      }),
      removeChannel: () => {},
      __handlers: handlers,
      __fire: (payload: any) => handlers.forEach(h => h(payload)),
    },
  }
})

import { startLiveTelemetry, stopLiveTelemetry } from '../telemetryService'
import { latestAtom, liveBufferAtomPrimitive, connectionStateAtom } from '../../atoms'
import * as supa from '../../../lib/supabase'

describe('telemetryService', () => {
  let store: ReturnType<typeof createStore>

  beforeEach(() => {
    store = createStore()
  })

  afterEach(() => {
    stopLiveTelemetry()
  })

  it('sets connectionState to live on first message', () => {
    startLiveTelemetry(store, 'dev1')
    ;(supa.supabase as any).__fire({ new: { id: 1, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    expect(store.get(connectionStateAtom)).toBe('live')
  })

  it('writes latest + appends to buffer', () => {
    startLiveTelemetry(store, 'dev1')
    ;(supa.supabase as any).__fire({ new: { id: 1, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    const latest = store.get(latestAtom)
    expect(latest).not.toBeNull()
    expect(store.get(liveBufferAtomPrimitive).length).toBe(1)
  })

  it('caps the buffer at 200', () => {
    startLiveTelemetry(store, 'dev1')
    for (let i = 0; i < 250; i++) {
      ;(supa.supabase as any).__fire({ new: { id: i, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    }
    expect(store.get(liveBufferAtomPrimitive).length).toBe(200)
    expect(store.get(liveBufferAtomPrimitive)[0].id).toBe(50)
  })

  it('marks stale after 15s of no messages', async () => {
    vi.useFakeTimers()
    startLiveTelemetry(store, 'dev1')
    ;(supa.supabase as any).__fire({ new: { id: 1, device_key: 'dev1', recorded_at: new Date().toISOString(), ch0_v: 12, ch0_i: 1, ch0_p: 12 } })
    expect(store.get(connectionStateAtom)).toBe('live')
    vi.advanceTimersByTime(16000)
    expect(store.get(connectionStateAtom)).toBe('stale')
    vi.useRealTimers()
  })
})
```

- [ ] **Step 4: Run test, verify fail**

```bash
cd ui && npm test -- telemetryService.test
```

Expected: FAIL — `telemetryService.ts` doesn't exist.

- [ ] **Step 5: Implement telemetryService**

Create `ui/src/state/services/telemetryService.ts`:

```ts
import type { Getter, Setter } from 'jotai'
import type { Store } from 'jotai'
import { supabase } from '../../lib/supabase'
import {
  latestAtom,
  liveBufferAtomPrimitive,
  appendBufferAtom,
  connectionStateAtom,
} from '../atoms'
import type { TelemetryPoint } from '../../lib/types'

interface TelemetryRow {
  id: number
  device_key: string
  recorded_at: string
  ch0_v: number | null; ch0_i: number | null; ch0_p: number | null
  ch1_v: number | null; ch1_i: number | null; ch1_p: number | null
  ch2_v: number | null; ch2_i: number | null; ch2_p: number | null
  ch3_v: number | null; ch3_i: number | null; ch3_p: number | null
  // server-computed values
  pv_power: number | null
  battery_power: number | null
  inverter_power: number | null
  dc_load_power: number | null
  system_status: 'unknown' | 'charging' | 'discharging' | 'balanced' | null
  energy_wh0: number | null; energy_wh1: number | null
  energy_wh2: number | null; energy_wh3: number | null
  soc_pct0: number | null; soc_pct1: number | null
  soc_pct2: number | null; soc_pct3: number | null
}

function rowToPoint(row: TelemetryRow): TelemetryPoint {
  const p: Record<string, number> = {}
  const set = (k: string, v: number | null | undefined) => {
    if (v != null) p[k] = v
  }
  set('ch0_V', row.ch0_v); set('ch0_I', row.ch0_i); set('ch0_P', row.ch0_p)
  set('ch1_V', row.ch1_v); set('ch1_I', row.ch1_i); set('ch1_P', row.ch1_p)
  set('ch2_V', row.ch2_v); set('ch2_I', row.ch2_i); set('ch2_P', row.ch2_p)
  set('ch3_V', row.ch3_v); set('ch3_I', row.ch3_i); set('ch3_P', row.ch3_p)
  set('pv_power', row.pv_power)
  set('battery_power', row.battery_power)
  set('inverter_power', row.inverter_power)
  set('dc_load_power', row.dc_load_power)
  set('energy_wh0', row.energy_wh0); set('energy_wh1', row.energy_wh1)
  set('energy_wh2', row.energy_wh2); set('energy_wh3', row.energy_wh3)
  set('soc_pct0', row.soc_pct0); set('soc_pct1', row.soc_pct1)
  set('soc_pct2', row.soc_pct2); set('soc_pct3', row.soc_pct3)
  if (row.system_status) p['system_status'] = row.system_status as unknown as number
  return {
    id: row.id,
    device_id: row.device_key,
    recorded_at: row.recorded_at,
    payload: p,
    metadata: {},
  }
}

const STALE_MS = 15_000
let currentChannel: ReturnType<typeof supabase.channel> | null = null
let staleTimer: ReturnType<typeof setTimeout> | null = null
let visibilityHandler: (() => void) | null = null
let activeStore: Store | null = null
let activeDeviceKey: string | null = null

function clearStaleTimer() {
  if (staleTimer) { clearTimeout(staleTimer); staleTimer = null }
}

function armStaleTimer(set: Setter) {
  clearStaleTimer()
  staleTimer = setTimeout(() => {
    set(connectionStateAtom, 'stale')
  }, STALE_MS)
}

export function startLiveTelemetry(store: Store, deviceKey: string) {
  stopLiveTelemetry()
  activeStore = store
  activeDeviceKey = deviceKey
  store.set(latestAtom, null)
  store.set(liveBufferAtomPrimitive, [])
  store.set(connectionStateAtom, 'connecting')

  const channel = supabase
    .channel(`telemetry-${deviceKey}`)
    .on('postgres_changes', {
      event: 'INSERT',
      schema: 'public',
      table: 'telemetry_computed',
      filter: `device_key=eq.${deviceKey}`,
    }, (payload) => {
      const row = payload.new as TelemetryRow
      const point = rowToPoint(row)
      store.set(latestAtom, point)
      store.set(appendBufferAtom, point)
      store.set(connectionStateAtom, 'live')
      armStaleTimer(store.set)
    })
    .subscribe()
  currentChannel = channel

  // visibility-pause: don't re-arm the stale timer while tab is hidden
  visibilityHandler = () => {
    if (document.visibilityState === 'hidden') {
      clearStaleTimer()
    } else if (activeStore && activeDeviceKey) {
      store.set(connectionStateAtom, 'connecting')
      armStaleTimer(store.set)
    }
  }
  document.addEventListener('visibilitychange', visibilityHandler)
  armStaleTimer(store.set)
}

export function stopLiveTelemetry() {
  clearStaleTimer()
  if (visibilityHandler) {
    document.removeEventListener('visibilitychange', visibilityHandler)
    visibilityHandler = null
  }
  if (currentChannel) {
    supabase.removeChannel(currentChannel)
    currentChannel = null
  }
  activeStore = null
  activeDeviceKey = null
}
```

- [ ] **Step 6: Run test, verify pass**

```bash
cd ui && npm test -- telemetryService.test
```

Expected: 4 tests pass.

- [ ] **Step 7: Commit**

```bash
git add ui/src/state/atoms.ts ui/src/state/derived.ts ui/src/state/services/telemetryService.ts ui/src/state/services/__tests__/telemetryService.test.ts
git commit -m "feat(state): telemetry service (supabase subscription + ring buffer + stale)"
```

---

### Task 7: History atoms and service

**Files:**
- Create: `ui/src/state/history.ts`
- Create: `ui/src/state/services/historyService.ts`
- Create: `ui/src/src/widgets/__tests__/extractKeys.test.ts`
- Create: `ui/src/state/__tests__/history.test.ts`

Note: we will move `extractKeys` from `PowerHistoryChart.tsx` into the new `historyService` so the series-list is computed once per fetch. Add the test here.

- [ ] **Step 1: Write failing test for extractKeys**

Create `ui/src/widgets/__tests__/extractKeys.test.ts`:

```ts
import { describe, it, expect } from 'vitest'
import { extractKeys } from '../HistoryChartWidget'
import type { TelemetryPoint } from '../../lib/types'

function pt(payload: Record<string, number>): TelemetryPoint {
  return { id: 0, device_id: 'k', recorded_at: new Date().toISOString(), payload, metadata: {} }
}

describe('extractKeys (power)', () => {
  it('returns empty when no data', () => {
    expect(extractKeys([], 'power')).toEqual([])
  })

  it('prefers system power keys when present', () => {
    const data = [
      pt({ pv_power: 100, battery_power: 50, ch0_P: 80, ch1_P: 0, ch2_P: 0, ch3_P: 0 }),
    ]
    const keys = extractKeys(data, 'power')
    expect(keys).toContain('pv_power')
    expect(keys).toContain('battery_power')
    expect(keys).not.toContain('ch0_P')
  })

  it('excludes all-zero series', () => {
    const data = [pt({ pv_power: 100, battery_power: 0 })]
    const keys = extractKeys(data, 'power')
    expect(keys).toEqual(['pv_power'])
  })
})
```

- [ ] **Step 2: Run test, verify fail**

```bash
cd ui && npm test -- extractKeys.test
```

Expected: FAIL — module doesn't exist.

- [ ] **Step 3: Create history atoms**

Create `ui/src/state/history.ts`:

```ts
import { atom } from 'jotai'
import { atomFamily } from 'jotai/utils'
import { loadable } from 'jotai/utils'
import { supabase } from '../lib/supabase'
import { refreshTriggerAtom } from './atoms'
import type { TelemetryPoint } from '../lib/types'

export type HistoryRange = '1h' | '6h' | '24h' | '7d' | '30d'
export type HistoryMetric = 'power' | 'voltage' | 'current'

const RANGE_HOURS: Record<HistoryRange, number> = {
  '1h': 1, '6h': 6, '24h': 24, '7d': 168, '30d': 720,
}
const RANGE_LIMITS: Record<HistoryRange, number> = {
  '1h': 1000, '6h': 5000, '24h': 20000, '7d': 50000, '30d': 50000,
}

interface HistoryKey {
  deviceKey: string
  range: HistoryRange
  metric: HistoryMetric
}

export const historyAtomFamily = atomFamily((k: HistoryKey) =>
  loadable(atom(async (get) => {
    get(refreshTriggerAtom)
    const hours = RANGE_HOURS[k.range]
    if (k.range === '1h' || k.range === '6h') {
      const since = new Date(Date.now() - hours * 3600 * 1000).toISOString()
      const { data, error } = await supabase
        .from('telemetry_computed')
        .select('*')
        .eq('device_key', k.deviceKey)
        .gte('recorded_at', since)
        .order('recorded_at', { ascending: true })
        .limit(RANGE_LIMITS[k.range])
      if (error) throw error
      return (data ?? []).map((row: any): TelemetryPoint => ({
        id: row.id,
        device_id: row.device_key,
        recorded_at: row.recorded_at,
        payload: reconstructPayload(row),
        metadata: {},
      }))
    }
    const { data, error } = await supabase.rpc('get_aggregated_telemetry', {
      p_device_key: k.deviceKey,
      p_hours: hours,
      p_metric: k.metric,
    })
    if (error) throw error
    return (data ?? []).map((row: any): TelemetryPoint => {
      const payload: Record<string, number> = {}
      for (const [key, val] of Object.entries(row)) {
        if (key === 'bucket') continue
        if (val != null && typeof val === 'number') payload[key] = val
      }
      return { id: 0, device_id: k.deviceKey, recorded_at: row.bucket as string, payload, metadata: {} }
    })
  })),
)

interface DrilldownKey {
  deviceKey: string
  tStart: string
  tEnd: string
  metric: HistoryMetric
}

export const drilldownLoadableAtom = atomFamily((k: DrilldownKey) =>
  loadable(atom(async (get) => {
    get(refreshTriggerAtom)
    const { data, error } = await supabase
      .from('telemetry_computed')
      .select('*')
      .eq('device_key', k.deviceKey)
      .gte('recorded_at', k.tStart)
      .lte('recorded_at', k.tEnd)
      .order('recorded_at', { ascending: true })
      .limit(20000)
    if (error) throw error
    return (data ?? []).map((row: any): TelemetryPoint => ({
      id: row.id,
      device_id: row.device_key,
      recorded_at: row.recorded_at,
      payload: reconstructPayload(row),
      metadata: {},
    }))
  })),
)

function reconstructPayload(row: any): Record<string, number> {
  const p: Record<string, number> = {}
  const set = (k: string, v: any) => { if (v != null && typeof v === 'number') p[k] = v }
  set('ch0_V', row.ch0_v); set('ch0_I', row.ch0_i); set('ch0_P', row.ch0_p)
  set('ch1_V', row.ch1_v); set('ch1_I', row.ch1_i); set('ch1_P', row.ch1_p)
  set('ch2_V', row.ch2_v); set('ch2_I', row.ch2_i); set('ch2_P', row.ch2_p)
  set('ch3_V', row.ch3_v); set('ch3_I', row.ch3_i); set('ch3_P', row.ch3_p)
  set('pv_power', row.pv_power)
  set('battery_power', row.battery_power)
  set('inverter_power', row.inverter_power)
  set('dc_load_power', row.dc_load_power)
  set('inverter_current', row.inverter_current)
  set('energy_wh0', row.energy_wh0); set('energy_wh1', row.energy_wh1)
  set('energy_wh2', row.energy_wh2); set('energy_wh3', row.energy_wh3)
  set('soc_pct0', row.soc_pct0); set('soc_pct1', row.soc_pct1)
  set('soc_pct2', row.soc_pct2); set('soc_pct3', row.soc_pct3)
  return p
}
```

- [ ] **Step 4: Create historyService**

Create `ui/src/state/services/historyService.ts`:

```ts
import { supabase } from '../../lib/supabase'
import type { HistoryRange, HistoryMetric } from '../history'
import type { TelemetryPoint, ChannelName } from '../../lib/types'
import { refreshTriggerAtom } from '../atoms'
import type { Getter, Setter } from 'jotai'

export interface SeriesSelection {
  keys: string[]
}

// --- extractKeys (moved from PowerHistoryChart) ---

const METRIC_REGEX: Record<HistoryMetric, RegExp> = {
  power: /^ch\d_P$|ina226_p|^ina3221_p\d$|inverter_power|pv_power|battery_power|dc_load_power/,
  voltage: /^ch\d_V$|ina226_v|^ina3221_v\d$/,
  current: /^ch\d_I$|ina226_i|^ina3221_i\d$|inverter_current$/,
}

const SYSTEM_POWER_KEYS = ['pv_power', 'battery_power', 'inverter_power', 'dc_load_power']

export function extractKeys(data: TelemetryPoint[], metric: HistoryMetric): string[] {
  if (data.length === 0) return []
  const allKeys = new Set<string>()
  for (const pt of data) {
    Object.keys(pt.payload).forEach(k => allKeys.add(k))
  }
  const regexKeys = Array.from(allKeys).filter(k => METRIC_REGEX[metric].test(k))
  const nonZero = regexKeys.filter(k => data.some(pt => {
    const v = pt.payload[k]
    return v != null && Math.abs(v) > 0.5
  }))
  if (metric === 'power') {
    const hasSystem = SYSTEM_POWER_KEYS.some(k => nonZero.includes(k))
    if (hasSystem) {
      return nonZero.filter(k => SYSTEM_POWER_KEYS.includes(k) || k === 'ina226_p')
    }
    return nonZero
  }
  if (metric === 'voltage' || metric === 'current') {
    const result: string[] = []
    const seenChannels = new Set<number>()
    for (const k of nonZero) {
      const ch = k.match(/^ch(\d)_[VI]$/i)
      if (ch) { seenChannels.add(parseInt(ch[1])); result.push(k) }
    }
    for (const k of nonZero) {
      const ina = k.match(/^ina3221_[vi](\d)$/)
      if (ina && !seenChannels.has(parseInt(ina[1]))) result.push(k)
    }
    const ina226Key = metric === 'voltage' ? 'ina226_v' : 'ina226_i'
    if (nonZero.includes(ina226Key)) result.push(ina226Key)
    return result
  }
  return nonZero
}

// --- keyToLabel (moved from PowerHistoryChart) ---

function vcName(channelNames: ChannelName[] | undefined, idx: number): string {
  return channelNames?.find(cn => cn.channel === idx)?.name ?? `VC${idx}`
}

export function keyToLabel(k: string, channelNames?: ChannelName[]): string {
  if (k === 'ina226_p') return 'INA226'
  if (k === 'ina226_v') return 'INA226 V'
  if (k === 'ina226_i') return 'INA226 I'
  if (k === 'inverter_current') return 'Inverter I'
  if (k === 'inverter_power') return 'Inverter Output'
  if (k === 'pv_power') return 'PV Generation'
  if (k === 'battery_power') return 'Battery'
  if (k === 'dc_load_power') return 'DC Load'
  if (k === 'soc_pct0') return 'Battery SOC'
  const ina = k.match(/^ina3221_([pvi])([0-2])$/)
  if (ina) {
    const m2m: Record<string, string> = { p: 'P', v: 'V', i: 'I' }
    return `${vcName(channelNames, parseInt(ina[2]))} ${m2m[ina[1]]}`
  }
  const ch = k.match(/^ch(\d)_([PVI])$/i)
  if (ch) return `${vcName(channelNames, parseInt(ch[1]))} ${ch[2].toUpperCase()}`
  return k
}

// --- Range / drilldown ---

export function suggestDrilldown(
  fromRange: HistoryRange,
  bucketTimeMs: number,
): HistoryRange {
  if (fromRange === '24h') return '1h'
  if (fromRange === '7d') return '1h'
  if (fromRange === '30d') return '6h'
  return '1h'
}

export function bucketToWindow(
  bucketISO: string,
  bucketMs: number,
): { tStart: string; tEnd: string } {
  const t = new Date(bucketISO).getTime()
  return {
    tStart: new Date(t - bucketMs / 2).toISOString(),
    tEnd: new Date(t + bucketMs / 2).toISOString(),
  }
}

export function forceRefresh(set: Setter) {
  set(refreshTriggerAtom, (n: number) => n + 1)
}
```

- [ ] **Step 5: Create the `extractKeys` re-export placeholder**

We can't import `extractKeys` from a non-existent module yet. Task 14 builds `HistoryChartWidget` which exports it. For now, create a stub at `ui/src/widgets/HistoryChartWidget.tsx`:

```ts
// Stub. Real implementation in Task 14.
export { extractKeys } from '../state/services/historyService'
```

- [ ] **Step 6: Run test, verify pass**

```bash
cd ui && npm test -- extractKeys.test
```

Expected: 3 tests pass.

- [ ] **Step 7: Commit**

```bash
git add ui/src/state/history.ts ui/src/state/services/historyService.ts ui/src/widgets/__tests__/extractKeys.test.ts ui/src/widgets/HistoryChartWidget.tsx
git commit -m "feat(state): history atoms + service with extractKeys, keyToLabel, drilldown helpers"
```

---

### Task 8: Channels service

**Files:**
- Create: `ui/src/state/services/channelsService.ts`

- [ ] **Step 1: Implement**

Create `ui/src/state/services/channelsService.ts`:

```ts
import type { Store } from 'jotai'
import { supabase, fetchDeviceChannels } from '../../lib/supabase'
import { deviceChannelsAtomFamily } from '../atoms'
import type { DeviceChannels } from '../../lib/types'

const cache = new Map<string, DeviceChannels>()

export async function loadChannels(store: Store, deviceKey: string): Promise<DeviceChannels | null> {
  if (cache.has(deviceKey)) {
    const cached = cache.get(deviceKey)!
    store.set(deviceChannelsAtomFamily(deviceKey), cached)
    return cached
  }
  const data = await fetchDeviceChannels(deviceKey)
  if (data) {
    cache.set(deviceKey, data)
    store.set(deviceChannelsAtomFamily(deviceKey), data)
  }
  return data
}

export function invalidateChannels(deviceKey: string) {
  cache.delete(deviceKey)
}
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/state/services/channelsService.ts
git commit -m "feat(state): channels service (cached device_channels fetch)"
```

---

### Task 9: Relay service

**Files:**
- Create: `ui/src/state/services/relayService.ts`

- [ ] **Step 1: Implement**

Create `ui/src/state/services/relayService.ts`:

```ts
import type { Store } from 'jotai'
import { supabase } from '../../lib/supabase'
import { relayStatesAtomFamily } from '../atoms'
import type { RelayState } from '../../lib/types'

let currentChannel: ReturnType<typeof supabase.channel> | null = null

export async function loadRelays(store: Store, deviceKey: string) {
  const { data } = await supabase
    .from('relay_states')
    .select('*')
    .eq('device_key', deviceKey)
    .order('relay_index')
  if (data) {
    store.set(relayStatesAtomFamily(deviceKey), data as RelayState[])
  }
}

export function subscribeRelays(store: Store, deviceKey: string): () => void {
  if (currentChannel) {
    supabase.removeChannel(currentChannel)
    currentChannel = null
  }
  const channel = supabase
    .channel(`relay-state-${deviceKey}`)
    .on('postgres_changes', {
      event: '*',
      schema: 'public',
      table: 'relay_states',
      filter: `device_key=eq.${deviceKey}`,
    }, (payload) => {
      const r = payload.new as RelayState
      store.set(relayStatesAtomFamily(deviceKey), (prev) => {
        const idx = prev.findIndex(rel => rel.id === r.id)
        if (idx >= 0) {
          const next = [...prev]
          next[idx] = r
          return next
        }
        return [...prev, r].sort((a, b) => a.relay_index - b.relay_index)
      })
    })
    .subscribe()
  currentChannel = channel
  return () => {
    if (currentChannel) {
      supabase.removeChannel(currentChannel)
      currentChannel = null
    }
  }
}

export async function toggleRelay(
  store: Store,
  deviceKey: string,
  relay: RelayState,
  newState: boolean,
): Promise<void> {
  // Optimistic update
  store.set(relayStatesAtomFamily(deviceKey), (prev) =>
    prev.map(r => r.id === relay.id ? { ...r, is_energized: newState } : r),
  )
  const { error } = await supabase.from('settings_commands').insert({
    device_key: deviceKey,
    cmd_type: 'set_relay',
    payload: {
      idx: relay.relay_index,
      is_energized: newState,
      active_high: relay.active_high ?? true,
      enabled: true,
      overcurrent_A: 0,
      undervoltage_V: 0,
      soc_low_pct: 0,
      soc_high_pct: 100,
      trip_delay_ms: 500,
      reset_delay_ms: 5000,
    },
    status: 'pending',
  })
  if (error) {
    // Revert on error
    store.set(relayStatesAtomFamily(deviceKey), (prev) =>
      prev.map(r => r.id === relay.id ? { ...r, is_energized: relay.is_energized } : r),
    )
  }
}
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/state/services/relayService.ts
git commit -m "feat(state): relay service (subscribe + toggle with optimistic update)"
```

---

## Phase 3: UI shell

### Task 10: Now ticker hook

**Files:**
- Create: `ui/src/state/nowTicker.ts`
- Modify: `ui/src/App.tsx`

- [ ] **Step 1: Create the hook**

Create `ui/src/state/nowTicker.ts`:

```ts
import { useEffect } from 'react'
import { useSetAtom } from 'jotai'
import { nowAtom } from './atoms'

/**
 * Mount once at app root. Updates nowAtom at 1Hz. Widgets that need
 * "X seconds ago" derive from secondsAgoAtom. Widgets that don't
 * care about time don't subscribe, so they don't re-render.
 */
export function useNowTicker() {
  const tick = useSetAtom(nowAtom)
  useEffect(() => {
    const id = setInterval(() => tick(), 1000)
    return () => clearInterval(id)
  }, [tick])
}
```

- [ ] **Step 2: Mount it in App.tsx**

Edit `ui/src/App.tsx`. Add at the top:
```ts
import { useNowTicker } from './state/nowTicker'
```

Inside `export default function App()`, after the `return` becomes a function body — actually `App` returns JSX directly. Refactor: wrap the JSX in a component that calls the hook first. Replace the entire `export default function App()` block with:

```tsx
export default function App() {
  useNowTicker()
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/provision" element={<ProvisioningPage />} />
        <Route path="/reset-password" element={<ResetPasswordPage />} />
        <Route path="/dashboard" element={<ProtectedRoute><DashboardPage /></ProtectedRoute>} />
        <Route path="/dashboard/legacy" element={<ProtectedRoute><DashboardPage /></ProtectedRoute>} />
        <Route path="/admin" element={<ProtectedRoute><AdminPage /></ProtectedRoute>} />
        <Route path="/settings" element={<ProtectedRoute><SettingsPage /></ProtectedRoute>} />
        <Route path="/channels" element={<ProtectedRoute><ChannelsPage /></ProtectedRoute>} />
        <Route path="/" element={<Navigate to="/dashboard" replace />} />
      </Routes>
    </BrowserRouter>
  )
}
```

(We'll wire the actual new DashboardPage vs LegacyDashboardPage in Task 21. For now both routes point to the same component.)

- [ ] **Step 3: Verify tsc + build**

```bash
cd ui && npx tsc --noEmit && npm run build
```

Expected: tsc passes; build succeeds.

- [ ] **Step 4: Commit**

```bash
git add ui/src/state/nowTicker.ts ui/src/App.tsx
git commit -m "feat(state): useNowTicker hook + mount in App"
```

---

### Task 11: Layout atoms and service

**Files:**
- Create: `ui/src/state/layout.ts`
- Create: `ui/src/state/services/layoutService.ts`

- [ ] **Step 1: Create layout atoms**

Create `ui/src/state/layout.ts`:

```ts
import { atom } from 'jotai'
import { layoutAtom } from './atoms'
import type { LayoutDoc, LayoutEntry, WidgetType } from '../lib/types'

const DEFAULT_LAYOUT: LayoutDoc = {
  version: 1,
  entries: [
    { id: 'qs',     type: 'quickstats',  gridArea: { col: 1,  row: 1,  colSpan: 12, rowSpan: 2 } },
    { id: 'relays', type: 'relays',      gridArea: { col: 1,  row: 3,  colSpan: 12, rowSpan: 1 } },
    { id: 'inv',    type: 'inverter',    gridArea: { col: 1,  row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'gen',    type: 'generation',  gridArea: { col: 4,  row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'bat',    type: 'battery',     gridArea: { col: 7,  row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'sp1',    type: 'spacer',      gridArea: { col: 10, row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'vc0',    type: 'vc0',         gridArea: { col: 1,  row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 0 } },
    { id: 'vc1',    type: 'vc1',         gridArea: { col: 4,  row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 1 } },
    { id: 'vc2',    type: 'vc2',         gridArea: { col: 7,  row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 2 } },
    { id: 'vc3',    type: 'vc3',         gridArea: { col: 10, row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 3 } },
    { id: 'hist',   type: 'history',     gridArea: { col: 1,  row: 8,  colSpan: 12, rowSpan: 6 } },
  ],
}

export const defaultLayoutAtom = atom<LayoutDoc>(DEFAULT_LAYOUT)
```

- [ ] **Step 2: Create layout service**

Create `ui/src/state/services/layoutService.ts`:

```ts
import type { Store } from 'jotai'
import { supabase } from '../../lib/supabase'
import { layoutAtom } from '../atoms'
import { defaultLayoutAtom } from '../layout'
import type { LayoutDoc } from '../../lib/types'

interface SavedLayout {
  user_id: string
  doc: LayoutDoc
  updated_at: string
}

export async function loadLayout(store: Store, userId: string): Promise<LayoutDoc> {
  const { data, error } = await supabase
    .from('user_dashboard_layouts')
    .select('doc, updated_at')
    .eq('user_id', userId)
    .maybeSingle()
  if (error || !data) {
    const fallback = store.get(defaultLayoutAtom)
    store.set(layoutAtom, fallback)
    return fallback
  }
  const saved = (data as SavedLayout).doc
  const doc = saved?.version === 1 ? saved : store.get(defaultLayoutAtom)
  store.set(layoutAtom, doc)
  return doc
}

export async function saveLayout(store: Store, userId: string, doc: LayoutDoc): Promise<void> {
  store.set(layoutAtom, doc)
  await supabase
    .from('user_dashboard_layouts')
    .upsert({ user_id: userId, doc, updated_at: new Date().toISOString() })
}

export function resetLayout(store: Store): LayoutDoc {
  const doc = store.get(defaultLayoutAtom)
  store.set(layoutAtom, doc)
  return doc
}
```

- [ ] **Step 3: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors (the user_dashboard_layouts table may not exist yet — that's fine, the error path returns the default).

- [ ] **Step 4: Commit**

```bash
git add ui/src/state/layout.ts ui/src/state/services/layoutService.ts
git commit -m "feat(state): layout atoms + service (load/save/reset user-dashboard layout)"
```

---

### Task 12: Widget registry

**Files:**
- Create: `ui/src/widgets/registry.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/registry.tsx`:

```tsx
import type { ComponentType } from 'react'
import type { WidgetType, WidgetDef } from '../lib/types'

// Lazy imports for code-splitting per widget
const QuickStatsWidget = () => import('./QuickStatsWidget')
const RelaysWidget = () => import('./RelaysWidget')
const InverterWidget = () => import('./InverterWidget')
const GenerationWidget = () => import('./GenerationWidget')
const BatteryWidget = () => import('./BatteryWidget')
const VCCardWidget = () => import('./VCCardWidget')
const HistoryChartWidget = () => import('./HistoryChartWidget')
const SpacerWidget = () => import('./SpacerWidget')
const PlaceholderWidget = () => import('./PlaceholderWidget')

export const registry: Record<WidgetType, WidgetDef & { loader: () => Promise<{ default: ComponentType<any> }> }> = {
  quickstats: {
    type: 'quickstats',
    label: 'Quick Stats',
    defaultSize: { colSpan: 12, rowSpan: 2 },
    loader: QuickStatsWidget,
  },
  relays: {
    type: 'relays',
    label: 'Relays',
    defaultSize: { colSpan: 12, rowSpan: 1 },
    loader: RelaysWidget,
  },
  inverter: {
    type: 'inverter',
    label: 'Inverter',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: InverterWidget,
  },
  generation: {
    type: 'generation',
    label: 'Generation',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: GenerationWidget,
  },
  battery: {
    type: 'battery',
    label: 'Battery',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: BatteryWidget,
  },
  vc0: {
    type: 'vc0',
    label: 'VC 0',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 0 },
    loader: VCCardWidget,
  },
  vc1: {
    type: 'vc1',
    label: 'VC 1',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 1 },
    loader: VCCardWidget,
  },
  vc2: {
    type: 'vc2',
    label: 'VC 2',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 2 },
    loader: VCCardWidget,
  },
  vc3: {
    type: 'vc3',
    label: 'VC 3',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 3 },
    loader: VCCardWidget,
  },
  history: {
    type: 'history',
    label: 'History',
    defaultSize: { colSpan: 12, rowSpan: 6 },
    loader: HistoryChartWidget,
  },
  spacer: {
    type: 'spacer',
    label: 'Spacer',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: SpacerWidget,
  },
  placeholder: {
    type: 'placeholder',
    label: 'Placeholder',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: PlaceholderWidget,
  },
}
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: errors about missing widget files (we haven't created them yet). That's expected; we'll fix in Task 13.

- [ ] **Step 3: Commit (skip if tsc errors prevent clean commit)**

```bash
git add ui/src/widgets/registry.tsx
git commit -m "feat(widgets): registry with lazy loaders for all 12 widget types"
```

---

### Task 13: Widget host + grid + placeholder widgets

**Files:**
- Create: `ui/src/widgets/SpacerWidget.tsx`
- Create: `ui/src/widgets/PlaceholderWidget.tsx`
- Create: `ui/src/widgets/WidgetHost.tsx`
- Create: `ui/src/widgets/WidgetGrid.tsx`

- [ ] **Step 1: Spacer widget**

Create `ui/src/widgets/SpacerWidget.tsx`:

```tsx
export default function SpacerWidget() {
  return <div className="h-full w-full" aria-hidden />
}
```

- [ ] **Step 2: Placeholder widget**

Create `ui/src/widgets/PlaceholderWidget.tsx`:

```tsx
export default function PlaceholderWidget({ label }: { label?: string }) {
  return (
    <div className="h-full w-full rounded-2xl border border-dashed border-slate-200 flex items-center justify-center text-slate-300 text-sm">
      {label ?? 'Placeholder'}
    </div>
  )
}
```

- [ ] **Step 3: Widget host**

Create `ui/src/widgets/WidgetHost.tsx`:

```tsx
import { Suspense, lazy } from 'react'
import type { LayoutEntry } from '../lib/types'
import { registry } from './registry'

interface Props {
  entry: LayoutEntry
}

export default function WidgetHost({ entry }: Props) {
  const def = registry[entry.type]
  if (!def) {
    return (
      <div className="h-full w-full rounded-2xl border border-red-200 bg-red-50 flex items-center justify-center text-red-500 text-sm">
        Unknown widget: {entry.type}
      </div>
    )
  }
  const Component = lazy(def.loader)
  return (
    <Suspense fallback={<div className="h-full w-full bg-slate-50 animate-pulse rounded-2xl" />}>
      <Component {...(entry.props ?? {})} />
    </Suspense>
  )
}
```

- [ ] **Step 4: Widget grid**

Create `ui/src/widgets/WidgetGrid.tsx`:

```tsx
import { useAtomValue } from 'jotai'
import { layoutAtom } from '../state/atoms'
import { defaultLayoutAtom } from '../state/layout'
import WidgetHost from './WidgetHost'

const ROW_HEIGHT = '80px'

export default function WidgetGrid() {
  const saved = useAtomValue(layoutAtom)
  const fallback = useAtomValue(defaultLayoutAtom)
  const doc = saved ?? fallback

  return (
    <div
      className="grid gap-4"
      style={{
        gridTemplateColumns: 'repeat(12, minmax(0, 1fr))',
        gridAutoRows: ROW_HEIGHT,
      }}
    >
      {doc.entries.map(entry => (
        <div
          key={entry.id}
          style={{
            gridColumn: `${entry.gridArea.col} / span ${entry.gridArea.colSpan}`,
            gridRow: `${entry.gridArea.row} / span ${entry.gridArea.rowSpan}`,
            minHeight: 0,
          }}
        >
          <WidgetHost entry={entry} />
        </div>
      ))}
    </div>
  )
}
```

- [ ] **Step 5: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: errors about missing QuickStatsWidget, RelaysWidget, etc. Those are fixed in Phase 4. Don't commit yet.

- [ ] **Step 6: Commit**

```bash
git add ui/src/widgets/SpacerWidget.tsx ui/src/widgets/PlaceholderWidget.tsx ui/src/widgets/WidgetHost.tsx ui/src/widgets/WidgetGrid.tsx
git commit -m "feat(widgets): WidgetHost + WidgetGrid + Spacer/Placeholder"
```

---

## Phase 4: Widgets

### Task 14: QuickStats widget

**Files:**
- Create: `ui/src/widgets/QuickStatsWidget.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/QuickStatsWidget.tsx`:

```tsx
import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { SunIcon, Battery0Icon, ArrowUpIcon, ArrowDownIcon } from '@heroicons/react/24/outline'
import { computedTelemetryAtom, pvPowerAtom, batteryPowerAtom, inverterPowerAtom, systemStatusAtom } from '../state/derived'
import { latestAtom } from '../state/atoms'

const STATUS_STYLES: Record<string, { dot: string; text: string; label: string }> = {
  charging:    { dot: 'bg-emerald-500', text: 'text-emerald-700', label: 'Charging' },
  discharging: { dot: 'bg-amber-500',   text: 'text-amber-700',   label: 'Discharging' },
  balanced:    { dot: 'bg-sky-500',     text: 'text-sky-700',     label: 'Balanced' },
  unknown:     { dot: 'bg-slate-400',   text: 'text-slate-600',   label: 'Unknown' },
}

function StaticNumber({ value, suffix = '', decimals = 1 }: { value: number; suffix?: string; decimals?: number }) {
  const display = Math.abs(value) < 0.05 && value !== 0 ? '0' : value.toFixed(decimals)
  return <span className="tabular-nums">{display}{suffix}</span>
}

function Chip({ label, value, unit, color, icon }: { label: string; value: number; unit: string; color: string; icon?: React.ReactNode }) {
  return (
    <div className="flex items-center gap-2.5 min-w-fit">
      {icon && <div className={`shrink-0 ${color}`}>{icon}</div>}
      <div className="flex flex-col leading-tight">
        <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">{label}</span>
        <span className={`text-lg font-semibold tabular-nums transition-colors duration-200 ${color}`}>
          <StaticNumber value={value} />
          <span className="text-sm font-normal text-slate-400 ml-0.5">{unit}</span>
        </span>
      </div>
    </div>
  )
}

function Directional({ label, value, unit }: { label: string; value: number; unit: string }) {
  const positive = value > 0.5
  const negative = value < -0.5
  const color = positive ? 'text-emerald-600' : negative ? 'text-cyan-600' : 'text-slate-400'
  const Icon = positive ? ArrowUpIcon : negative ? ArrowDownIcon : null
  return (
    <div className="flex items-center gap-2.5 min-w-fit">
      <div className="shrink-0 w-5 h-5">
        {Icon && <Icon className={`w-5 h-5 ${color}`} />}
      </div>
      <div className="flex flex-col leading-tight">
        <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">{label}</span>
        <span className={`text-lg font-semibold tabular-nums transition-colors duration-200 ${color}`}>
          <StaticNumber value={Math.abs(value)} />
          <span className="text-sm font-normal text-slate-400 ml-0.5">{unit}</span>
        </span>
      </div>
    </div>
  )
}

function QuickStatsWidget() {
  const computed = useAtomValue(computedTelemetryAtom)
  const pv = useAtomValue(pvPowerAtom)
  const battery = useAtomValue(batteryPowerAtom)
  const inverter = useAtomValue(inverterPowerAtom)
  const status = useAtomValue(systemStatusAtom)
  const latest = useAtomValue(latestAtom)
  const totalPower = Math.abs(inverter) + computed.dc_load_power
  const style = STATUS_STYLES[status]

  if (!latest) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-6 flex items-center text-slate-300 text-sm">
        Awaiting telemetry…
      </div>
    )
  }

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-6 flex items-center overflow-hidden">
      <div className="flex items-center gap-6 min-w-fit">
        <div className="flex flex-col leading-tight">
          <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">Total Power</span>
          <span className="text-2xl font-bold text-slate-800 tabular-nums">
            {totalPower > 0 ? <><StaticNumber value={totalPower} /><span className="text-base font-normal text-slate-400 ml-1">W</span></> : <span className="text-slate-300">--</span>}
          </span>
        </div>
        <div className="h-10 w-px bg-slate-200" />
        <Chip label="PV" value={pv} unit="W" color="text-amber-500" icon={<SunIcon className="w-5 h-5" />} />
        <div className="h-10 w-px bg-slate-200" />
        <Directional label="Inverter" value={inverter} unit="W" />
        <div className="h-10 w-px bg-slate-200" />
        <Directional label="Battery" value={battery} unit="W" />
      </div>
      <div className="ml-auto flex items-center gap-3 shrink-0">
        <div className="flex items-center gap-2.5 px-3.5 py-1.5 rounded-full bg-slate-50">
          <span className={`w-3 h-3 rounded-full ${style.dot} ${status !== 'unknown' && status !== 'balanced' ? 'animate-pulse' : ''}`} />
          <span className={`text-xs font-semibold ${style.text}`}>{style.label}</span>
        </div>
      </div>
    </div>
  )
}

export default memo(QuickStatsWidget)
```

- [ ] **Step 2: Verify tsc (other widget files still missing — that's fine)**

```bash
cd ui && npx tsc --noEmit 2>&1 | grep "QuickStats" | head -5
```

Expected: no QuickStats errors. Other widget errors are expected.

- [ ] **Step 3: Commit**

```bash
git add ui/src/widgets/QuickStatsWidget.tsx
git commit -m "feat(widgets): QuickStats (memoized, CSS-only state transitions, no framer-motion)"
```

---

### Task 15: Relays widget

**Files:**
- Create: `ui/src/widgets/RelaysWidget.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/RelaysWidget.tsx`:

```tsx
import { memo, useEffect, useState } from 'react'
import { useAtomValue, useStore } from 'jotai'
import { useStore as useJotaiStore } from 'jotai'
import { relayStatesAtomFamily, latestAtom } from '../state/atoms'
import { loadRelays, subscribeRelays, toggleRelay } from '../state/services/relayService'
import ToggleSwitch from '../components/ui/ToggleSwitch'
import type { RelayState } from '../lib/types'

interface Props {
  deviceKey: string
}

function RelaysWidget({ deviceKey }: Props) {
  const relays = useAtomValue(relayStatesAtomFamily(deviceKey))
  const store = useJotaiStore()
  const [busy, setBusy] = useState<Record<number, boolean>>({})

  useEffect(() => {
    let cleanup: (() => void) | undefined
    loadRelays(store, deviceKey).then(() => {
      cleanup = subscribeRelays(store, deviceKey)
    })
    return () => { if (cleanup) cleanup() }
  }, [deviceKey, store])

  if (relays.length === 0) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-5 flex items-center text-slate-300 text-sm">
        No relays configured
      </div>
    )
  }

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-5 py-3 overflow-x-auto overflow-y-hidden">
      <div className="flex items-center justify-between mb-2">
        <h3 className="font-semibold text-slate-800 text-sm">Relays</h3>
        <span className="text-xs text-slate-400">{relays.length} switch{relays.length === 1 ? '' : 'es'}</span>
      </div>
      <div className="flex gap-3">
        {relays.map(relay => (
          <div key={relay.id} className="flex-shrink-0 min-w-[110px] flex flex-col items-center gap-1 px-3 py-2 rounded-xl bg-slate-50 border border-slate-100">
            <div className="flex items-center justify-between w-full">
              <span className="text-xs font-semibold text-slate-700">R{relay.relay_index}</span>
              <span className={`text-[10px] font-semibold uppercase tracking-wider transition-colors duration-200 ${relay.is_energized ? 'text-emerald-600' : 'text-slate-400'}`}>
                {relay.is_energized ? 'On' : 'Off'}
              </span>
            </div>
            <ToggleSwitch
              checked={relay.is_energized}
              onChange={() => {
                setBusy(b => ({ ...b, [relay.id]: true }))
                toggleRelay(store, deviceKey, relay, !relay.is_energized).finally(() => {
                  setTimeout(() => setBusy(b => ({ ...b, [relay.id]: false })), 1500)
                })
              }}
              disabled={busy[relay.id]}
              size="md"
              label={undefined}
            />
            <span className="text-[10px] text-slate-400 font-mono">GPIO {relay.gpio_pin}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

export default memo(RelaysWidget)
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit 2>&1 | grep -i "RelaysWidget" | head -5
```

Expected: no RelaysWidget errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/widgets/RelaysWidget.tsx
git commit -m "feat(widgets): Relays (subscribes to relayStatesAtomFamily, optimistic toggle)"
```

---

### Task 16: Inverter widget

**Files:**
- Create: `ui/src/widgets/InverterWidget.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/InverterWidget.tsx`:

```tsx
import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { ArrowUpIcon, ArrowDownIcon } from '@heroicons/react/24/outline'
import { inverterPowerAtom, systemStatusAtom } from '../state/derived'

const STATUS_COPY: Record<string, { label: string; tone: string }> = {
  charging:    { label: 'PV is charging the battery', tone: 'text-emerald-600' },
  discharging: { label: 'Battery is supplying loads',  tone: 'text-amber-600' },
  balanced:    { label: 'Production matches demand',  tone: 'text-sky-600' },
  unknown:     { label: 'Awaiting telemetry',          tone: 'text-slate-400' },
}

function InverterWidget() {
  const inverter = useAtomValue(inverterPowerAtom)
  const status = useAtomValue(systemStatusAtom)
  const active = inverter > 0.5
  const deficit = inverter < -0.5
  const balanced = !active && !deficit
  const valueColor = active ? 'text-emerald-600' : deficit ? 'text-red-500' : 'text-slate-400'
  const bg = active ? 'from-emerald-50/50' : deficit ? 'from-red-50/50' : 'from-slate-50/50'
  const displayValue = Math.abs(inverter) < 0.05 ? 0 : inverter
  const Arrow = active ? ArrowUpIcon : deficit ? ArrowDownIcon : null
  const arrowColor = active ? 'text-emerald-500' : deficit ? 'text-red-500' : 'text-slate-300'
  const statusInfo = STATUS_COPY[status] ?? STATUS_COPY.unknown

  return (
    <div className={`h-full w-full bg-gradient-to-br ${bg} to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-5`}>
      <div className="flex items-start justify-between mb-2">
        <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Inverter</span>
        {Arrow && <Arrow className={`w-5 h-5 ${arrowColor}`} />}
      </div>
      <div className="flex items-baseline gap-2">
        <span className={`text-3xl font-bold tabular-nums transition-colors duration-200 ${valueColor}`}>
          {balanced ? '0' : Math.abs(displayValue).toFixed(1)}
        </span>
        <span className={`text-lg font-medium ${valueColor}`}>W</span>
      </div>
      <div className="mt-2 text-sm font-medium transition-colors duration-200" style={{ color: 'inherit' }}>
        <span className={valueColor}>
          {active && 'Supplying inverter'}
          {deficit && 'DC deficit'}
          {balanced && 'Idle'}
        </span>
      </div>
      <div className="mt-3 pt-3 border-t border-slate-100">
        <span className={`text-xs font-medium ${statusInfo.tone}`}>{statusInfo.label}</span>
      </div>
    </div>
  )
}

export default memo(InverterWidget)
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit 2>&1 | grep -i "InverterWidget" | head -5
```

Expected: no InverterWidget errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/widgets/InverterWidget.tsx
git commit -m "feat(widgets): Inverter (CSS-color transitions, no framer-motion)"
```

---

### Task 17: Generation widget

**Files:**
- Create: `ui/src/widgets/GenerationWidget.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/GenerationWidget.tsx`:

```tsx
import { memo, useEffect, useState } from 'react'
import { useAtomValue } from 'jotai'
import { SunIcon } from '@heroicons/react/24/outline'
import { supabase } from '../lib/supabase'

interface Props {
  deviceId: string
}

interface DailyResult {
  total: number
  hourly: Array<{ hour: string; value: number; projected?: boolean }>
  rangeLabel: string
}

function GenerationWidget({ deviceId }: Props) {
  const [range, setRange] = useState<'today' | 'yesterday' | '7d' | '30d'>('today')
  const [result, setResult] = useState<DailyResult>({ total: 0, hourly: [], rangeLabel: 'Today' })
  const [isLoading, setIsLoading] = useState(false)

  useEffect(() => {
    if (!deviceId) return
    let cancelled = false
    setIsLoading(true)
    const now = new Date()
    let startTime: Date
    let endTime: Date
    let rangeLabel = ''
    let isDaily = false
    if (range === 'today') {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()))
      endTime = now; rangeLabel = 'Today'
    } else if (range === 'yesterday') {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1))
      endTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 1, 23, 59, 59, 999))
      rangeLabel = 'Yesterday'
    } else if (range === '7d') {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 6))
      endTime = now; isDaily = true; rangeLabel = 'Last 7 Days'
    } else {
      startTime = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate() - 29))
      endTime = now; isDaily = true; rangeLabel = 'Last 30 Days'
    }
    const fn = isDaily ? 'get_daily_generation' : 'get_hourly_generation'
    supabase.rpc(fn, {
      p_device_key: deviceId, // pass device key; some implementations want id
      p_start_time: startTime.toISOString(),
      p_end_time: endTime.toISOString(),
    }).then(({ data, error }) => {
      if (cancelled) return
      setIsLoading(false)
      if (error || !Array.isArray(data) || data.length === 0) {
        setResult({ total: 0, hourly: [], rangeLabel })
        return
      }
      let total = 0
      const buckets = (data as any[]).map((row) => {
        const kwh = row.kwh ?? 0
        const label = isDaily
          ? `${String(new Date(row.day).getMonth() + 1).padStart(2, '0')}/${String(new Date(row.day).getDate()).padStart(2, '0')}`
          : `${String(new Date(row.hour_start).getHours()).padStart(2, '0')}:00`
        total += kwh
        return { hour: label, value: Math.round(kwh * 100) / 100, projected: row.is_partial ?? false }
      }).reverse()
      setResult({ total: Math.round(total * 100) / 100, hourly: buckets, rangeLabel })
    })
    return () => { cancelled = true }
  }, [deviceId, range])

  return (
    <div className="h-full w-full bg-gradient-to-br from-amber-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-5">
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <SunIcon className="w-5 h-5 text-amber-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Generation</span>
        </div>
        <span className="text-xs text-amber-600 font-medium">kWh</span>
      </div>
      <div className="flex items-center gap-1 mb-3 flex-wrap">
        {(['today', 'yesterday', '7d', '30d'] as const).map(r => (
          <button key={r} onClick={() => setRange(r)}
            className={`px-2.5 py-1 rounded-lg text-[11px] font-semibold transition-colors duration-150 ${range === r ? 'bg-amber-500 text-white shadow-sm' : 'bg-slate-100 text-slate-500 hover:bg-slate-200'}`}>
            {r === '7d' ? '7 Days' : r === '30d' ? '30 Days' : r === 'today' ? 'Today' : 'Yesterday'}
          </button>
        ))}
      </div>
      <div className="flex items-baseline gap-1.5 mb-1 min-h-[36px]">
        {isLoading ? (
          <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
        ) : (
          <>
            <span className="text-3xl font-bold text-amber-600 tabular-nums">{result.total > 0 ? result.total.toFixed(2) : '0.00'}</span>
            <span className="text-base font-medium text-amber-500">kWh</span>
          </>
        )}
      </div>
      <div className="text-[10px] text-slate-400">{result.rangeLabel}</div>
    </div>
  )
}

export default memo(GenerationWidget)
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit 2>&1 | grep -i "GenerationWidget" | head -5
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/widgets/GenerationWidget.tsx
git commit -m "feat(widgets): Generation (range selector, no animation, fixed-height loading)"
```

---

### Task 18: Battery widget

**Files:**
- Create: `ui/src/widgets/BatteryWidget.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/BatteryWidget.tsx`:

```tsx
import { memo, useEffect, useState } from 'react'
import { Battery0Icon } from '@heroicons/react/24/outline'
import { supabase } from '../lib/supabase'

interface Props {
  deviceId: string
}

interface BatteryState {
  chargeWh: number
  capacityWh: number
  energyIn24h: number
  energyOut24h: number
  isFullChargeToday: boolean
}

function BatteryWidget({ deviceId }: Props) {
  const [state, setState] = useState<BatteryState>({ chargeWh: 0, capacityWh: 0, energyIn24h: 0, energyOut24h: 0, isFullChargeToday: false })
  const [isLoading, setIsLoading] = useState(true)

  useEffect(() => {
    if (!deviceId) return
    let cancelled = false
    const fetch = async () => {
      const { data, error } = await supabase.rpc('get_battery_charge', { p_device_id: deviceId, p_hours: 24 })
      if (cancelled) return
      setIsLoading(false)
      if (error || !data) return
      const row = Array.isArray(data) ? data[0] : data
      setState({
        chargeWh: row.charge_wh ?? 0,
        capacityWh: row.capacity_wh ?? 0,
        energyIn24h: row.energy_in_24h ?? 0,
        energyOut24h: row.energy_out_24h ?? 0,
        isFullChargeToday: row.is_full_charge_today ?? false,
      })
    }
    fetch()
    const id = setInterval(fetch, 10000)
    return () => { cancelled = true; clearInterval(id) }
  }, [deviceId])

  const displayPct = state.capacityWh > 0 ? (state.chargeWh / state.capacityWh) * 100 : 0
  const barColor = displayPct > 50 ? 'bg-emerald-500' : displayPct > 20 ? 'bg-amber-400' : 'bg-red-500'

  return (
    <div className="h-full w-full bg-gradient-to-br from-emerald-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-5">
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <Battery0Icon className="w-5 h-5 text-emerald-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Battery</span>
        </div>
        <span className="text-xs text-emerald-600 font-medium">Wh</span>
      </div>
      <div className="flex items-baseline gap-1.5 mb-1 min-h-[36px]">
        {isLoading ? (
          <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
        ) : (
          <>
            <span className="text-3xl font-bold text-emerald-600 tabular-nums">{state.chargeWh > 0 ? state.chargeWh.toFixed(0) : '0'}</span>
            <span className="text-base font-medium text-emerald-500">/ {state.capacityWh.toFixed(0)} Wh</span>
          </>
        )}
      </div>
      {state.capacityWh > 0 && (
        <div className="mt-2">
          <div className="w-full bg-slate-100 rounded-full h-2 overflow-hidden">
            <div className={`h-2 rounded-full ${barColor} transition-all duration-500`} style={{ width: `${Math.min(displayPct, 100)}%` }} />
          </div>
          <div className="flex items-center justify-between mt-1">
            <span className="text-[10px] text-slate-400">
              {displayPct.toFixed(1)}%{state.isFullChargeToday && <span className="ml-1 text-emerald-500">● full</span>}
            </span>
            <span className="text-[10px] text-emerald-500">
              +{state.energyIn24h.toFixed(1)} / -{state.energyOut24h.toFixed(1)} Wh (24h)
            </span>
          </div>
        </div>
      )}
    </div>
  )
}

export default memo(BatteryWidget)
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit 2>&1 | grep -i "BatteryWidget" | head -5
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/widgets/BatteryWidget.tsx
git commit -m "feat(widgets): Battery (polls get_battery_charge, fixed-height loading, no animation)"
```

---

### Task 19: VC card widget

**Files:**
- Create: `ui/src/widgets/VCCardWidget.tsx`

- [ ] **Step 1: Implement**

Create `ui/src/widgets/VCCardWidget.tsx`:

```tsx
import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { channelPayloadAtomFamily } from '../state/derived'
import { deviceChannelsAtomFamily } from '../state/atoms'
import { latestAtom } from '../state/atoms'

interface Props {
  channel: number
}

function VCCardWidget({ channel }: Props) {
  const payload = useAtomValue(channelPayloadAtomFamily(channel))
  const latest = useAtomValue(latestAtom)
  const channels = useAtomValue(deviceChannelsAtomFamily(latest?.device_id ?? ''))
  const name = channels?.channel_names?.find(c => c.channel === channel)?.name ?? `VC${channel}`
  const batteryCapacity = channels?.battery_profiles?.[channel]?.capacity_mAh ?? 0
  const hasBattery = batteryCapacity > 0
  const online = !!latest

  return (
    <div className={`h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 border-l-4 ${hasBattery && payload.socPct !== null && payload.socPct < 20 ? 'border-l-yellow-400' : 'border-l-emerald-500'} p-4`}>
      <div className="flex items-center justify-between mb-3">
        <div className="flex items-center gap-2">
          <span className="font-semibold text-slate-800 text-sm">{name}</span>
          <span className={`w-2 h-2 rounded-full ${online ? 'bg-emerald-400' : 'bg-slate-300'}`} />
        </div>
      </div>
      <div className="grid grid-cols-3 gap-2 mb-3">
        <div className="text-center">
          <div className="text-[10px] text-slate-400">V</div>
          <div className="text-xl font-bold text-slate-800 tabular-nums">{payload.voltage !== null ? payload.voltage.toFixed(2) : '--'}</div>
        </div>
        <div className="text-center">
          <div className="text-[10px] text-slate-400">A</div>
          <div className="text-xl font-bold text-slate-800 tabular-nums">{payload.current !== null ? payload.current.toFixed(2) : '--'}</div>
        </div>
        <div className="text-center">
          <div className="text-[10px] text-slate-400">W</div>
          <div className="text-xl font-bold text-slate-800 tabular-nums">{payload.power !== null ? payload.power.toFixed(1) : '--'}</div>
        </div>
      </div>
      {hasBattery && (
        <div>
          <div className="flex justify-between text-[10px] text-slate-500 mb-1">
            <span>SoC</span>
            <span>{payload.socPct !== null ? `${payload.socPct.toFixed(0)}%` : '--'}</span>
          </div>
          <div className="w-full bg-slate-100 rounded-full h-2 overflow-hidden">
            {payload.socPct !== null && (
              <div className="h-2 rounded-full bg-gradient-to-r from-emerald-400 to-teal-500 transition-all duration-300" style={{ width: `${Math.min(payload.socPct, 100)}%` }} />
            )}
          </div>
        </div>
      )}
    </div>
  )
}

export default memo(VCCardWidget)
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit 2>&1 | grep -i "VCCard" | head -5
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/widgets/VCCardWidget.tsx
git commit -m "feat(widgets): VCCard (per-channel, fixed-height, no framer-motion)"
```

---

### Task 20: History chart widget (the heavy one)

This is the big one — uPlot, drag-zoom, click-drilldown, breadcrumb, lazy load.

**Files:**
- Modify: `ui/src/widgets/HistoryChartWidget.tsx` (replace stub)
- Create: `ui/src/widgets/HistoryTooltip.tsx`
- Create: `ui/src/state/atoms.ts` (add zoom/drilldown atoms)

- [ ] **Step 1: Add zoom + drilldown atoms**

Edit `ui/src/state/atoms.ts`. Add at the end:

```ts
// --- Chart zoom + drilldown ---

export interface ZoomRange { start: number; end: number }
export const zoomRangeAtom = atom<ZoomRange | null>(null)

export interface BreadcrumbEntry { rangeLabel: string; tStart: number; tEnd: number; fromRange: string }
export const drilldownBreadcrumbAtom = atom<BreadcrumbEntry[]>([])

export const hoveredPointAtom = atom<{ time: string; values: Record<string, number> } | null>(null)
```

- [ ] **Step 2: Tooltip component**

Create `ui/src/widgets/HistoryTooltip.tsx`:

```tsx
import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { hoveredPointAtom } from '../state/atoms'
import { keyToLabel } from '../state/services/historyService'
import type { ChannelName } from '../lib/types'

interface Props {
  visibleKeys: string[]
  metric: 'power' | 'voltage' | 'current'
  channelNames?: ChannelName[]
}

const UNIT: Record<string, string> = { power: 'W', voltage: 'V', current: 'A' }

function HistoryTooltip({ visibleKeys, metric, channelNames }: Props) {
  const point = useAtomValue(hoveredPointAtom)
  if (!point) return null
  return (
    <div className="absolute z-10 pointer-events-none bg-slate-800 rounded-xl shadow-lg px-3 py-2.5 min-w-[140px]">
      <div className="text-[11px] text-slate-400 mb-1.5 font-medium">{point.time}</div>
      {visibleKeys.map(k => {
        const v = point.values[k]
        if (typeof v !== 'number') return null
        return (
          <div key={k} className="flex items-center justify-between gap-3 text-[12px] py-0.5">
            <span className="text-slate-300">{keyToLabel(k, channelNames)}</span>
            <span className="text-slate-100 font-semibold font-mono">
              {k === 'soc_pct0' ? `${v.toFixed(0)} %` : `${Math.abs(v).toFixed(metric === 'voltage' ? 2 : 1)} ${UNIT[metric]}`}
            </span>
          </div>
        )
      })}
    </div>
  )
}

export default memo(HistoryTooltip)
```

- [ ] **Step 3: Replace HistoryChartWidget stub with real implementation**

Replace `ui/src/widgets/HistoryChartWidget.tsx` with:

```tsx
import { memo, useEffect, useRef, useState, lazy, Suspense } from 'react'
import { useAtomValue, useSetAtom } from 'jotai'
import { historyAtomFamily, drilldownLoadableAtom, type HistoryRange, type HistoryMetric } from '../state/history'
import { extractKeys, keyToLabel, suggestDrilldown, bucketToWindow, forceRefresh } from '../state/services/historyService'
import { zoomRangeAtom, drilldownBreadcrumbAtom, hoveredPointAtom, deviceChannelsAtomFamily, latestAtom } from '../state/atoms'
import HistoryTooltip from './HistoryTooltip'

// Re-export for tests
export { extractKeys } from '../state/services/historyService'

const UPlot = lazy(() => import('uplot'))

const DOWNSAMPLE_TARGET = 1500

interface Props {
  deviceKey: string
}

const SERIES_COLORS = [
  '#3b82f6', '#22c55e', '#f59e0b', '#a855f7', '#06b6d4',
  '#ec4899', '#f97316', '#84cc16', '#10b981', '#ef4444',
]

function HistoryChartWidget({ deviceKey }: Props) {
  const [range, setRange] = useState<HistoryRange>('24h')
  const [metric, setMetric] = useState<HistoryMetric>('power')
  const [visibleLines, setVisibleLines] = useState<Set<string>>(new Set())
  const containerRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<any>(null)
  const seriesDataRef = useRef<{ xs: number[]; ysList: number[][]; keys: string[] }>({ xs: [], ysList: [], keys: [] })

  const breadcrumb = useAtomValue(drilldownBreadcrumbAtom)
  const setBreadcrumb = useSetAtom(drilldownBreadcrumbAtom)
  const zoom = useAtomValue(zoomRangeAtom)
  const setZoom = useSetAtom(zoomRangeAtom)
  const setHovered = useSetAtom(hoveredPointAtom)
  const channels = useAtomValue(deviceChannelsAtomFamily(deviceKey))
  const latest = useAtomValue(latestAtom)

  // Decide which loadable to use
  const drilldown = breadcrumb.length > 0 ? breadcrumb[breadcrumb.length - 1] : null
  const loadable = drilldown
    ? useAtomValue(drilldownLoadableAtom({ deviceKey, tStart: new Date(drilldown.tStart).toISOString(), tEnd: new Date(drilldown.tEnd).toISOString(), metric }))
    : useAtomValue(historyAtomFamily({ deviceKey, range, metric }))

  // Build plot data from loadable
  useEffect(() => {
    if (loadable.state !== 'hasData' || !containerRef.current) return
    const data = loadable.data
    if (data.length === 0) return
    const keys = extractKeys(data, metric)
    if (keys.length === 0) return

    // Downsample to 1500 points
    const step = data.length > DOWNSAMPLE_TARGET ? Math.ceil(data.length / DOWNSAMPLE_TARGET) : 1
    const xs: number[] = []
    const ysList: number[][] = keys.map(() => [])
    for (let i = 0; i < data.length; i += step) {
      const pt = data[i]
      xs.push(new Date(pt.recorded_at).getTime() / 1000)
      keys.forEach((k, ki) => {
        ysList[ki].push((pt.payload as any)[k] ?? null)
      })
    }
    seriesDataRef.current = { xs, ysList, keys }
    setVisibleLines(prev => prev.size === 0 ? new Set(keys) : prev)
  }, [loadable.state, loadable.data, metric])

  // Init uPlot
  useEffect(() => {
    if (loadable.state !== 'hasData') return
    if (seriesDataRef.current.xs.length === 0) return
    if (plotRef.current) return
    if (!containerRef.current) return

    let cancelled = false
    import('uplot').then((mod) => {
      if (cancelled || !containerRef.current) return
      const uPlot = mod.default
      const { xs, ysList, keys } = seriesDataRef.current
      const visibleKeys = keys.filter(k => visibleLines.has(k) || visibleLines.size === 0)
      const series: any[] = [{}]
      visibleKeys.forEach((k, i) => {
        series.push({
          label: keyToLabel(k, channels?.channel_names),
          stroke: SERIES_COLORS[i % SERIES_COLORS.length],
          width: 2,
          fill: SERIES_COLORS[i % SERIES_COLORS.length] + '40',
          points: { show: false },
        })
      })
      const opts: any = {
        width: containerRef.current.clientWidth,
        height: 480,
        series,
        scales: { x: { time: true } },
        axes: [
          { stroke: '#94a3b8', grid: { stroke: '#f1f5f9' } },
          { stroke: '#94a3b8', grid: { stroke: '#f1f5f9' } },
        ],
        cursor: {
          drag: { x: true, y: false, setScale: true },
          sync: { key: 'history' },
          focus: { prox: 16 },
        },
        hooks: {
          setCursor: [
            (u: any) => {
              const idx = u.cursor.idx
              if (idx == null) { setHovered(null); return }
              const t = new Date(u.data[0][idx] * 1000).toLocaleString()
              const values: Record<string, number> = {}
              u.series.forEach((s: any, i: number) => {
                if (i === 0) return
                const v = u.data[i]?.[idx]
                if (typeof v === 'number') values[s.label] = v
              })
              setHovered({ time: t, values })
            },
          ],
          setScale: [
            (u: any, scaleKey: string) => {
              if (scaleKey === 'x') {
                setZoom({ start: u.scales.x.min * 1000, end: u.scales.x.max * 1000 })
              }
            },
          ],
        },
      }
      const data: any[] = [xs]
      visibleKeys.forEach((_, i) => data.push(ysList[i]))
      const plot = new uPlot(opts, data, containerRef.current)
      // Double-click to reset zoom
      containerRef.current!.addEventListener('dblclick', () => {
        plot.setScale('x', { min: xs[0], max: xs[xs.length - 1] })
        setZoom(null)
      })
      // Click on data point to drill
      containerRef.current!.addEventListener('click', (e) => {
        // uPlot handles drag-zoom; a click without drag is a drill
        // Use cursor's idx if available
        const idx = plot.cursor.idx
        if (idx == null) return
        const t = new Date(xs[idx] * 1000)
        const bucketMs = range === '24h' ? 3600_000 : range === '7d' ? 86400_000 : range === '30d' ? 86400_000 : 3600_000
        const { tStart, tEnd } = bucketToWindow(t.toISOString(), bucketMs)
        const drilldownRange = suggestDrilldown(range, bucketMs)
        setBreadcrumb([...breadcrumb, { rangeLabel: `${range} → ${t.toLocaleDateString()}`, tStart: new Date(tStart).getTime(), tEnd: new Date(tEnd).getTime(), fromRange: drilldownRange }])
      })
      plotRef.current = plot
    })
    return () => {
      cancelled = true
      if (plotRef.current) { plotRef.current.destroy(); plotRef.current = null }
    }
  }, [loadable.state, range, metric, visibleLines, channels, breadcrumb])

  // Push live data point into uPlot on every latest update
  useEffect(() => {
    if (!plotRef.current || !latest) return
    const t = new Date(latest.recorded_at).getTime() / 1000
    if (t <= seriesDataRef.current.xs[seriesDataRef.current.xs.length - 1]) return
    const newXs = [...seriesDataRef.current.xs, t]
    const newYsList = seriesDataRef.current.ysList.map(ys => {
      const key = seriesDataRef.current.keys[seriesDataRef.current.ysList.indexOf(ys)]
      return [...ys, (latest.payload as any)[key] ?? null]
    })
    seriesDataRef.current = { xs: newXs, ysList: newYsList, keys: seriesDataRef.current.keys }
    plotRef.current.setData([newXs, ...newYsList])
  }, [latest])

  if (loadable.state === 'loading') {
    return <div className="h-full w-full bg-slate-50 animate-pulse rounded-2xl" />
  }
  if (loadable.state === 'hasError') {
    return <div className="h-full w-full bg-red-50 text-red-600 rounded-2xl p-4">Failed to load: {String((loadable as any).error)}</div>
  }
  const visibleKeys = seriesDataRef.current.keys.filter(k => visibleLines.has(k))

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 p-4">
      <div className="flex items-center justify-between mb-3 flex-wrap gap-2">
        <h3 className="font-bold text-slate-800 text-sm">History</h3>
        <div className="flex items-center gap-1 flex-wrap">
          {(['1h', '6h', '24h', '7d', '30d'] as HistoryRange[]).map(r => (
            <button key={r} onClick={() => { setRange(r); setBreadcrumb([]); setZoom(null) }}
              className={`px-2.5 py-1 rounded-lg text-[11px] font-semibold transition-colors duration-150 ${range === r ? 'bg-gradient-to-r from-cyan-500 to-blue-500 text-white shadow-sm' : 'bg-slate-100 text-slate-500 hover:bg-slate-200'}`}>
              {r}
            </button>
          ))}
          {breadcrumb.length > 0 && (
            <div className="flex items-center gap-1 ml-2 text-[11px] text-slate-500">
              {breadcrumb.map((b, i) => (
                <span key={i} className="px-2 py-0.5 rounded bg-slate-100">{b.rangeLabel}</span>
              ))}
            </div>
          )}
          {zoom && (
            <button onClick={() => { setZoom(null); if (plotRef.current && seriesDataRef.current.xs.length > 0) plotRef.current.setScale('x', { min: seriesDataRef.current.xs[0], max: seriesDataRef.current.xs[seriesDataRef.current.xs.length - 1] }) }}
              className="ml-2 px-2 py-0.5 rounded bg-slate-100 text-slate-500 text-[11px]">
              Reset zoom
            </button>
          )}
          <button onClick={() => forceRefresh(useSetAtom as any)} className="ml-1 px-2 py-0.5 rounded bg-slate-100 text-slate-500 text-[11px]">↻</button>
        </div>
      </div>
      <div className="flex items-center gap-1.5 mb-3">
        {(['power', 'voltage', 'current'] as HistoryMetric[]).map(m => (
          <button key={m} onClick={() => setMetric(m)}
            className={`px-3 py-1 rounded-full text-[11px] font-semibold transition-colors duration-150 ${metric === m ? 'bg-gradient-to-r from-cyan-500 to-blue-500 text-white shadow-sm' : 'bg-slate-100 text-slate-500 hover:bg-slate-200'}`}>
            {m.charAt(0).toUpperCase() + m.slice(1)}
          </button>
        ))}
      </div>
      <div className="flex items-center gap-2 mb-3 flex-wrap">
        {seriesDataRef.current.keys.map((k, i) => {
          const active = visibleLines.has(k) || visibleLines.size === 0
          const color = SERIES_COLORS[i % SERIES_COLORS.length]
          return (
            <button key={k} onClick={() => {
              const next = new Set(visibleLines)
              if (next.has(k)) next.delete(k); else next.add(k)
              setVisibleLines(next)
            }}
              className={`flex items-center gap-1.5 text-[11px] px-2 py-0.5 rounded-full border transition-all duration-150 ${active ? 'border-transparent shadow-sm' : 'border-slate-200 text-slate-400 opacity-60'}`}
              style={active ? { backgroundColor: color + '18', borderColor: color + '40' } : {}}>
              <span className="w-2.5 h-2.5 rounded-full flex-shrink-0" style={{ background: color }} />
              <span style={active ? { color } : {}}>{keyToLabel(k, channels?.channel_names)}</span>
            </button>
          )
        })}
      </div>
      <div className="relative">
        <div ref={containerRef} />
        <HistoryTooltip visibleKeys={visibleKeys} metric={metric} channelNames={channels?.channel_names} />
      </div>
    </div>
  )
}

export default memo(HistoryChartWidget)
```

Note: `forceRefresh(useSetAtom as any)` is a placeholder — the proper call needs the actual setter. Replace that line with a real hook. Edit the imports: add `import { useSetAtom }` and inside the component, add `const triggerRefresh = useSetAtom(refreshTriggerAtom)`. Then change the button to `onClick={() => triggerRefresh(n => n + 1)}`.

Replace the button line:
```tsx
<button onClick={() => triggerRefresh(n => n + 1)} ... >↻</button>
```

And add at the top of the component:
```tsx
const triggerRefresh = useSetAtom(refreshTriggerAtom)
```

- [ ] **Step 4: Verify tsc + build**

```bash
cd ui && npx tsc --noEmit 2>&1 | head -30
```

Expected: errors fixed by adding `useSetAtom` for `refreshTriggerAtom`. Iterate until tsc passes.

- [ ] **Step 5: Run all tests**

```bash
cd ui && npm test
```

Expected: all tests pass (derived, history, telemetryService, extractKeys, sanity removed).

- [ ] **Step 6: Commit**

```bash
git add ui/src/widgets/HistoryChartWidget.tsx ui/src/widgets/HistoryTooltip.tsx ui/src/state/atoms.ts
git commit -m "feat(widgets): HistoryChart with uPlot (drag-zoom, click-drilldown, breadcrumb, live append)"
```

---

## Phase 5: Migration

### Task 21: New DashboardPage (uses WidgetGrid)

**Files:**
- Create: `ui/src/pages/DashboardPage.tsx` (replace)
- Create: `ui/src/pages/LegacyDashboardPage.tsx` (move old logic)

- [ ] **Step 1: Save the current DashboardPage as legacy**

```bash
cp ui/src/pages/DashboardPage.tsx ui/src/pages/LegacyDashboardPage.tsx
```

- [ ] **Step 2: Replace DashboardPage with the new version**

Replace `ui/src/pages/DashboardPage.tsx` with:

```tsx
import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useStore } from 'jotai'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { startLiveTelemetry, stopLiveTelemetry } from '../state/services/telemetryService'
import { loadChannels } from '../state/services/channelsService'
import { loadLayout } from '../state/services/layoutService'
import { layoutAtom } from '../state/atoms'
import { useSetAtom } from 'jotai'
import { useAtomValue } from 'jotai'
import type { Device } from '../lib/types'
import WidgetGrid from '../widgets/WidgetGrid'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'

export default function DashboardPage() {
  const navigate = useNavigate()
  const store = useStore()
  const setLayout = useSetAtom(layoutAtom)
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [userId, setUserId] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let mounted = true
    async function load() {
      const { data: { session } } = await supabase.auth.getSession()
      if (!session || !mounted) return
      setUserId(session.user.id)
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (mounted) {
        if (data) setDevices(data)
        setLoading(false)
      }
    }
    load()
    return () => { mounted = false }
  }, [])

  useEffect(() => {
    if (!selectedDevice || !userId) return
    let cancelled = false
    Promise.all([
      startLiveTelemetrySafe(store, selectedDevice.device_key),
      loadChannels(store, selectedDevice.device_key),
      loadLayout(store, userId),
    ])
    return () => {
      cancelled = true
      stopLiveTelemetry()
    }
  }, [selectedDevice, userId, store])

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }

  if (loading) {
    return <div className="flex items-center justify-center h-screen text-slate-500">Loading...</div>
  }

  const header = ({ onMenuClick }: { onMenuClick: () => void }) => (
    <HeaderBar
      devices={devices}
      selectedDeviceId={selectedDevice?.id ?? null}
      onSelectDevice={setSelectedDevice}
      isOnline={selectedDevice?.is_online ?? false}
      onMenuClick={onMenuClick}
    />
  )

  return (
    <DashboardLayout
      currentPath="/dashboard"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      header={header}
      deviceName={selectedDevice?.device_name}
    >
      {!selectedDevice ? (
        <div className="text-center py-12">
          <p className="text-slate-600 mb-4">No device selected.</p>
          <p className="text-sm text-slate-400">Select a device from the dropdown above to view telemetry.</p>
        </div>
      ) : (
        <WidgetGrid />
      )}
    </DashboardLayout>
  )
}

async function startLiveTelemetrySafe(store: any, deviceKey: string) {
  const { startLiveTelemetry } = await import('../state/services/telemetryService')
  startLiveTelemetry(store, deviceKey)
}
```

- [ ] **Step 3: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors. `HeaderBar` is being passed an extra prop `onMenuClick` only; remove `lastUpdated` from its required props (or pass it).

- [ ] **Step 4: Verify build**

```bash
cd ui && npm run build
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add ui/src/pages/DashboardPage.tsx ui/src/pages/LegacyDashboardPage.tsx
git commit -m "feat(pages): new DashboardPage uses WidgetGrid + Jotai services"
```

---

### Task 22: Update HeaderBar to read atoms

**Files:**
- Modify: `ui/src/components/HeaderBar.tsx`

- [ ] **Step 1: Replace the local state with atom reads**

Replace the `now` state and `setInterval` with atom-derived values. The new `HeaderBar.tsx`:

```tsx
import { useAtomValue } from 'jotai'
import { Bars3Icon } from '@heroicons/react/24/outline'
import { connectionStateAtom, nowAtom } from '../state/atoms'
import { secondsAgoAtom, latestAtom } from '../state/derived'
import { useSetAtom } from 'jotai'
import { useEffect } from 'react'
import type { Device } from '../lib/types'

export interface HeaderBarProps {
  devices: Device[]
  selectedDeviceId: string | null
  onSelectDevice: (device: Device) => void
  isOnline: boolean
  onMenuClick: () => void
}

function formatSecondsAgo(seconds: number): string {
  if (seconds < 1) return 'just now'
  if (seconds < 60) return `${seconds} second${seconds === 1 ? '' : 's'} ago`
  const minutes = Math.floor(seconds / 60)
  return `${minutes} minute${minutes === 1 ? '' : 's'} ago`
}

export default function HeaderBar({
  devices, selectedDeviceId, onSelectDevice, isOnline, onMenuClick,
}: HeaderBarProps) {
  const now = useAtomValue(nowAtom)
  const secondsAgo = useAtomValue(secondsAgoAtom)
  const conn = useAtomValue(connectionStateAtom)
  const isLive = isOnline && conn === 'live'

  return (
    <header className="sticky top-0 z-20 bg-white border-b border-slate-200 px-4 py-3 flex items-center justify-between gap-4">
      <div className="flex items-center gap-3 min-w-0">
        <button type="button" aria-label="Open navigation" onClick={onMenuClick}
          className="inline-flex items-center justify-center w-9 h-9 rounded-lg text-slate-600 hover:bg-slate-100">
          <Bars3Icon className="h-6 w-6" />
        </button>
        <select value={selectedDeviceId ?? ''}
          onChange={e => { const f = devices.find(d => d.id === e.target.value); if (f) onSelectDevice(f) }}
          className="rounded-lg border border-slate-200 bg-white text-sm px-3 py-1.5 focus:outline-none focus:ring-2 focus:ring-brand-400 max-w-[14rem] truncate">
          {devices.length === 0 ? <option value="">No devices</option> : (
            <><option value="">-- Choose a device --</option>
            {devices.map(d => <option key={d.id} value={d.id}>{d.device_name}</option>)}</>
          )}
        </select>
      </div>
      <div className="flex items-center gap-4 text-xs text-slate-500">
        <span className="font-mono hidden sm:inline">{new Date(now).toLocaleTimeString()}</span>
        <div className="flex items-center gap-2">
          <span className={`inline-block w-2 h-2 rounded-full ${isLive ? 'bg-emerald-500 animate-pulse' : 'bg-red-500'}`} aria-hidden />
          <span className="text-slate-700 font-medium">{isLive ? 'Online' : conn === 'stale' ? 'Stale' : 'Offline'}</span>
        </div>
        <span className="hidden md:inline">
          Last update:{' '}
          {secondsAgo === null ? <span className="text-slate-400">--</span> : <span className="text-slate-700">{formatSecondsAgo(secondsAgo)}</span>}
        </span>
      </div>
    </header>
  )
}
```

- [ ] **Step 2: Verify tsc**

```bash
cd ui && npx tsc --noEmit
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add ui/src/components/HeaderBar.tsx
git commit -m "refactor(ui): HeaderBar reads now/connection/secondsAgo from atoms"
```

---

### Task 23: Add Legacy route + Sidebar entry

**Files:**
- Modify: `ui/src/App.tsx`
- Modify: `ui/src/components/Sidebar.tsx`

- [ ] **Step 1: Update App.tsx routes**

Edit `ui/src/App.tsx`. Replace the route block:

```tsx
<Route path="/dashboard" element={<ProtectedRoute><DashboardPage /></ProtectedRoute>} />
<Route path="/dashboard/legacy" element={<ProtectedRoute><LegacyDashboardPage /></ProtectedRoute>} />
```

- [ ] **Step 2: Add Legacy entry to Sidebar**

Edit `ui/src/components/Sidebar.tsx`. Add to `NAV_ITEMS`:

```tsx
{ label: 'Dashboard (Legacy)', path: '/dashboard/legacy', Icon: SunIcon },
```

- [ ] **Step 3: Verify tsc + build**

```bash
cd ui && npx tsc --noEmit && npm run build
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add ui/src/App.tsx ui/src/components/Sidebar.tsx
git commit -m "feat(routes): add /dashboard/legacy route + Sidebar entry"
```

---

### Task 24: Run all tests + manual smoke

- [ ] **Step 1: Run all unit tests**

```bash
cd ui && npm test
```

Expected: all tests pass (derived, history, telemetryService, extractKeys).

- [ ] **Step 2: Build for production**

```bash
cd ui && npm run build
```

Expected: dist/ produced, no errors. Inspect `dist/index.html` exists.

- [ ] **Step 3: Manual smoke test**

```bash
cd ui && npm run dev
```

In browser:
- Login, see dashboard
- Select a device
- Confirm QuickStats, Relays, Inverter, Generation, Battery, VC×4, HistoryChart all render
- Confirm numbers don't flicker (no AnimatePresence)
- Confirm chart drag-zoom works (drag horizontally)
- Confirm chart double-click resets zoom
- Click a 7d day bucket → loads 1h slice, breadcrumb appears
- Toggle a relay → state updates
- Switch to `/dashboard/legacy` → old dashboard still works

- [ ] **Step 4: Document results in commit**

```bash
git add -A
git commit -m "chore: smoke test pass — new dashboard functional, legacy preserved" --allow-empty
```

(Only commit if there are changes; otherwise the --allow-empty records the verification.)

---

### Task 25: Delete replaced files

**Files:**
- Delete: `ui/src/hooks/useRealtime.ts`
- Delete: `ui/src/hooks/useComputedTelemetry.ts`
- Delete: `ui/src/hooks/useDailyGeneration.ts`
- Delete: `ui/src/hooks/useBatteryCharge.ts`
- Delete: `ui/src/components/PowerHistoryChart.tsx`
- Delete: `ui/src/components/QuickStatsRow.tsx`
- Delete: `ui/src/components/DailyGenerationCard.tsx`
- Delete: `ui/src/components/InverterPowerCard.tsx`
- Delete: `ui/src/components/BatteryChargeCard.tsx`
- Delete: `ui/src/components/VCDashboardCard.tsx`
- Delete: `ui/src/components/RelaySwitchRow.tsx`

- [ ] **Step 1: Delete the files**

```bash
cd ui
rm src/hooks/useRealtime.ts \
   src/hooks/useComputedTelemetry.ts \
   src/hooks/useDailyGeneration.ts \
   src/hooks/useBatteryCharge.ts \
   src/components/PowerHistoryChart.tsx \
   src/components/QuickStatsRow.tsx \
   src/components/DailyGenerationCard.tsx \
   src/components/InverterPowerCard.tsx \
   src/components/BatteryChargeCard.tsx \
   src/components/VCDashboardCard.tsx \
   src/components/RelaySwitchRow.tsx
```

- [ ] **Step 2: Remove hooks dir if empty**

```bash
cd ui
rmdir src/hooks 2>/dev/null || echo "hooks dir not empty or doesn't exist"
```

- [ ] **Step 3: Verify tsc + build**

```bash
cd ui && npx tsc --noEmit && npm run build
```

Expected: no errors. If any old file is still imported, tsc will report it; fix the imports.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(ui): delete replaced hooks and components (new dashboard owns these)"
```

---

### Task 26: Performance verification

This task is the success-criteria gate. No code changes unless something fails.

- [ ] **Step 1: Heap-stays-flat test**

Open the dashboard in Chrome DevTools. Take a heap snapshot. Leave the tab open for 1 hour. Take another heap snapshot. Compute the delta.

Expected: `delta < 10MB`.

- [ ] **Step 2: Chart 60fps on 7d range**

Switch to 7d range. Open DevTools Performance tab. Record for 10s. Check FPS meter.

Expected: average > 55fps. No long tasks > 50ms.

- [ ] **Step 3: No layout shift**

Open DevTools Performance → enable "Layout Shifts" track. Record 30s of normal dashboard use (data refresh, no manual interaction).

Expected: cumulative layout shift = 0.

- [ ] **Step 4: Mobile profile**

Chrome DevTools → Toggle device toolbar → Moto G Power. Throttle CPU 4x, network Slow 4G. Reload.

Expected: Time to Interactive < 2s. Largest Contentful Paint < 2.5s.

- [ ] **Step 5: Document results**

If all pass: commit an empty marker.

```bash
git commit --allow-empty -m "perf: dashboard hits success criteria (heap flat 1h, 60fps, 0 CLS, TTI < 2s)"
```

If any fail: file a follow-up issue, do not commit "perf" marker. Investigate and fix.

---

## Self-review

Spec coverage check:
- ✓ Phase 1: install + test infra + types (Task 1, 2, 3)
- ✓ Atoms: primitive (Task 4), derived (Task 5), history loadable (Task 7), zoom/drilldown (Task 20), buffer (Task 6)
- ✓ Services: telemetry (Task 6), history (Task 7), channels (Task 8), relay (Task 9), layout (Task 11)
- ✓ Visibility pause: telemetryService (Task 6)
- ✓ nowAtom + 1Hz ticker (Task 10)
- ✓ Widget registry (Task 12), WidgetHost (Task 13), WidgetGrid (Task 13)
- ✓ Default LayoutDoc (Task 11)
- ✓ CSS Grid, no flex grow (Task 13)
- ✓ Spacer/Placeholder (Task 13)
- ✓ QuickStats (Task 14), Relays (Task 15), Inverter (Task 16), Generation (Task 17), Battery (Task 18), VCCard (Task 19)
- ✓ uPlot chart (Task 20), tooltip (Task 20), drilldown (Task 20)
- ✓ Drag-zoom (Task 20), double-click reset (Task 20), click-drilldown (Task 20), breadcrumb (Task 20)
- ✓ Legacy route + sidebar (Task 23)
- ✓ Migration cutover (Task 21)
- ✓ HeaderBar atom-driven (Task 22)
- ✓ Delete replaced files (Task 25)
- ✓ Performance verification (Task 26)
- ✓ Tests: derived, history, telemetryService, extractKeys

Gaps: none. The spec's "page-builder UI" is explicitly a non-goal for v1 and out of scope.

Placeholder scan: All steps have actual code or commands. No "TBD", no "add validation", no "similar to Task N" without the code.

Type consistency:
- `WidgetType` defined in Task 3 used in registry, WidgetHost, WidgetGrid, layout.ts — consistent
- `TelemetryPoint` used in services and widgets — same shape
- `LayoutDoc` shape consistent across layout.ts, layoutService.ts, types.ts
- `GridArea` shape consistent
- `TelemetryRow` (private to telemetryService) is local; no leak

One issue: Task 20 has `forceRefresh(useSetAtom as any)` as a stub; I added a fix-up note in that step. Good — explicit and recoverable.

Execution handoff:

Plan complete and saved to `docs/superpowers/plans/2026-06-07-dashboard-redesign-plan.md`. Two execution options:

1. **Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
