# New Responsive Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new responsive dashboard that feels like a power-station / inverter app on mobile and is detailed, feature-rich, and multi-column on desktop; preserve the existing widget-grid dashboard as "Classic"; and surface the app version in the sidebar menu.

**Architecture:** `/dashboard` renders a new `DashboardPage` that uses `useIsMobile()` to swap between a mobile inverter-app layout and a dense multi-column desktop layout. The existing widget-based dashboard is copied unchanged to `ClassicDashboardPage.tsx` and served at `/dashboard/classic`. Shared primitives (`BatteryRing`, `PowerFlowPill`, `DesktopSummaryCard`, etc.) keep both views consistent. The app version is centralized in `lib/version.ts` and shown as a badge in the sidebar header.

**Tech Stack:** React 18 + TypeScript + Vite, Tailwind CSS, Jotai, React Router v6, Vitest, Heroicons.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `ui/src/lib/version.ts` | Single source of truth for the UI app version string. |
| `ui/src/lib/useIsMobile.ts` | Hook that returns `true` when viewport is narrower than Tailwind's `md` breakpoint (768 px). |
| `ui/src/components/dashboard/BatteryRing.tsx` | Circular SVG battery SoC gauge used on both mobile and desktop. |
| `ui/src/components/dashboard/PowerFlowPill.tsx` | Mobile horizontal pill: PV → Battery → Load with direction and wattage. |
| `ui/src/components/dashboard/SummaryBadge.tsx` | Small numeric badge used in mobile and desktop summaries. |
| `ui/src/components/dashboard/MobileChannelCard.tsx` | One thumb-friendly channel row for the mobile view. |
| `ui/src/components/dashboard/MobileDashboard.tsx` | Vertical mobile layout: battery ring, power flow, summary badges, channel list. |
| `ui/src/components/dashboard/DesktopSummaryCard.tsx` | Feature-rich desktop summary card with icon, value, unit, and color theme. |
| `ui/src/components/dashboard/DesktopChannelCard.tsx` | Detailed multi-metric channel card for desktop. |
| `ui/src/components/dashboard/BatteryDetailPanel.tsx` | Desktop battery panel with ring and status. |
| `ui/src/components/dashboard/RelayStatusPanel.tsx` | Desktop/mobile relay state panel. |
| `ui/src/components/dashboard/DesktopDashboard.tsx` | Multi-column desktop layout: summary row, channel grid, side panels. |
| `ui/src/pages/ClassicDashboardPage.tsx` | Preserved copy of today's `DashboardPage.tsx`; serves the existing widget grid under `/dashboard/classic`. |
| `ui/src/pages/DashboardPage.tsx` | Rewritten to render the new responsive dashboard. |
| `ui/src/App.tsx` | Routing: `/dashboard` → new, `/dashboard/classic` → classic. |
| `ui/src/components/Sidebar.tsx` | Adds an app-version badge and updates nav labels. |
| `ui/src/lib/__tests__/version.test.ts` | Version constant unit test. |
| `ui/src/lib/__tests__/useIsMobile.test.ts` | Hook unit test with mocked `matchMedia`. |
| `ui/src/components/__tests__/Sidebar.test.tsx` | Sidebar renders nav items and version badge. |
| `ui/src/components/dashboard/__tests__/BatteryRing.test.tsx` | Gauge renders SoC value and status. |
| `ui/src/components/dashboard/__tests__/MobileDashboard.test.tsx` | Mobile view renders ring, flow pill, and channel cards. |
| `ui/src/components/dashboard/__tests__/DesktopDashboard.test.tsx` | Desktop view renders summary cards and channel cards. |
| `ui/src/pages/__tests__/ClassicDashboardPage.test.tsx` | Classic page still renders loading state. |
| `ui/src/pages/__tests__/DashboardPage.test.tsx` | DashboardPage picks mobile vs desktop view based on hook. |

---

### Task 1: Add a centralized app version constant

**Files:**
- Create: `ui/src/lib/version.ts`
- Test: `ui/src/lib/__tests__/version.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { describe, it, expect } from 'vitest'
import { APP_VERSION } from '../version'

describe('version', () => {
  it('exports a non-empty semantic version string', () => {
    expect(APP_VERSION).toMatch(/^\d+\.\d+\.\d+/)
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/lib/__tests__/version.test.ts
```

Expected: FAIL with `Cannot find module '../version' or its corresponding type declarations`.

- [ ] **Step 3: Write minimal implementation**

Create `ui/src/lib/version.ts`:

```ts
/**
 * Human-readable UI application version.
 * Bump this when the dashboard ships a new user-facing revision.
 */
export const APP_VERSION = '0.2.0'
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/lib/__tests__/version.test.ts
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/lib/version.ts ui/src/lib/__tests__/version.test.ts
git commit -m "feat(ui): add centralized app version constant"
```

---

### Task 2: Add a responsive breakpoint hook

**Files:**
- Create: `ui/src/lib/useIsMobile.ts`
- Test: `ui/src/lib/__tests__/useIsMobile.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook } from '@testing-library/react'
import { useIsMobile } from '../useIsMobile'

function setWidth(width: number) {
  Object.defineProperty(window, 'innerWidth', {
    writable: true,
    configurable: true,
    value: width,
  })
}

function mockMatchMedia(matches: boolean) {
  return vi.fn().mockImplementation((query: string) => ({
    matches,
    media: query,
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    dispatchEvent: vi.fn(),
  }))
}

describe('useIsMobile', () => {
  beforeEach(() => {
    vi.stubGlobal('matchMedia', mockMatchMedia(false))
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('returns true when viewport is narrower than 768 px', () => {
    setWidth(600)
    vi.stubGlobal('matchMedia', mockMatchMedia(true))
    const { result } = renderHook(() => useIsMobile())
    expect(result.current).toBe(true)
  })

  it('returns false when viewport is 768 px or wider', () => {
    setWidth(1024)
    vi.stubGlobal('matchMedia', mockMatchMedia(false))
    const { result } = renderHook(() => useIsMobile())
    expect(result.current).toBe(false)
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/lib/__tests__/useIsMobile.test.ts
```

Expected: FAIL with `Cannot find module '../useIsMobile'`.

- [ ] **Step 3: Write minimal implementation**

Create `ui/src/lib/useIsMobile.ts`:

```ts
import { useState, useEffect } from 'react'

const MOBILE_BREAKPOINT_PX = 768

export function useIsMobile(): boolean {
  const [isMobile, setIsMobile] = useState(() => {
    if (typeof window === 'undefined') return false
    return window.innerWidth < MOBILE_BREAKPOINT_PX
  })

  useEffect(() => {
    if (typeof window === 'undefined') return

    const media = window.matchMedia(`(max-width: ${MOBILE_BREAKPOINT_PX - 1}px)`)
    const update = () => setIsMobile(media.matches)

    update()
    media.addEventListener('change', update)
    return () => media.removeEventListener('change', update)
  }, [])

  return isMobile
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/lib/__tests__/useIsMobile.test.ts
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/lib/useIsMobile.ts ui/src/lib/__tests__/useIsMobile.test.ts
git commit -m "feat(ui): add useIsMobile breakpoint hook"
```

---

### Task 3: Preserve the existing dashboard as Classic

**Files:**
- Create: `ui/src/pages/ClassicDashboardPage.tsx`
- Modify: `ui/src/App.tsx:40-41`
- Modify: `ui/src/components/Sidebar.tsx:24-30`
- Test: `ui/src/pages/__tests__/ClassicDashboardPage.test.tsx` (light render test)

- [ ] **Step 1: Copy the existing dashboard to ClassicDashboardPage**

Create `ui/src/pages/ClassicDashboardPage.tsx` with the exact contents of the current `ui/src/pages/DashboardPage.tsx`, changing only the default export name:

```tsx
import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useStore } from 'jotai'
import { selectedDeviceAtom } from '../state/atoms'
import { supabase } from '../lib/supabase'
import { loadChannels } from '../state/services/channelsService'
import { loadLayout } from '../state/services/layoutService'
import type { Device } from '../lib/types'
import WidgetGrid from '../widgets/WidgetGrid'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'

export default function ClassicDashboardPage() {
  const navigate = useNavigate()
  const store = useStore()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
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
    Promise.all([
      startLiveTelemetrySafe(store, selectedDevice.device_key),
      loadChannels(store, selectedDevice.device_key),
      loadLayout(store, userId),
      startAggregatesPollingSafe(store),
    ])
    return () => {
      stopLiveTelemetrySafe()
      stopAggregatesPollingSafe()
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
      currentPath="/dashboard/classic"
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

async function stopLiveTelemetrySafe() {
  const { stopLiveTelemetry } = await import('../state/services/telemetryService')
  stopLiveTelemetry()
}

async function startAggregatesPollingSafe(store: any) {
  const { startAggregatesPolling } = await import('../state/services/aggregatesService')
  startAggregatesPolling(store)
}

async function stopAggregatesPollingSafe() {
  const { stopAggregatesPolling } = await import('../state/services/aggregatesService')
  stopAggregatesPolling()
}
```

- [ ] **Step 2: Wire the classic route in App.tsx**

Modify `ui/src/App.tsx`. Replace the existing dashboard imports and routes with:

```tsx
import DashboardPage from './pages/DashboardPage'
import ClassicDashboardPage from './pages/ClassicDashboardPage'
```

And update the routes block so the dashboard routes read:

```tsx
<Route path="/dashboard" element={<ProtectedRoute><DashboardPage /></ProtectedRoute>} />
<Route path="/dashboard/classic" element={<ProtectedRoute><ClassicDashboardPage /></ProtectedRoute>} />
```

- [ ] **Step 3: Update the sidebar navigation labels**

In `ui/src/components/Sidebar.tsx`, change `NAV_ITEMS` to:

```ts
const NAV_ITEMS: NavItem[] = [
  { label: 'Dashboard', path: '/dashboard', Icon: SunIcon },
  { label: 'Classic Dashboard', path: '/dashboard/classic', Icon: SunIcon },
  { label: 'Channels', path: '/channels', Icon: BoltIcon },
  { label: 'Settings', path: '/settings', Icon: Cog6ToothIcon },
  { label: 'Admin', path: '/admin', Icon: ShieldCheckIcon },
]
```

- [ ] **Step 4: Write a light render test for ClassicDashboardPage**

Create `ui/src/pages/__tests__/ClassicDashboardPage.test.tsx`:

```tsx
import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import { Provider } from 'jotai'
import ClassicDashboardPage from '../ClassicDashboardPage'

vi.mock('../../lib/supabase', () => ({
  supabase: {
    auth: {
      getSession: vi.fn().mockResolvedValue({ data: { session: null } }),
      signOut: vi.fn(),
    },
    from: vi.fn().mockReturnValue({
      select: vi.fn().mockReturnValue({ order: vi.fn().mockResolvedValue({ data: [] }) }),
    }),
  },
}))

describe('ClassicDashboardPage', () => {
  it('renders a loading state before session is known', async () => {
    render(
      <BrowserRouter>
        <Provider>
          <ClassicDashboardPage />
        </Provider>
      </BrowserRouter>
    )
    expect(await screen.findByText('Loading...')).toBeInTheDocument()
  })
})
```

- [ ] **Step 5: Run the test and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/pages/__tests__/ClassicDashboardPage.test.tsx
npm run build
```

Expected: Tests pass and TypeScript build succeeds.

- [ ] **Step 6: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/pages/ClassicDashboardPage.tsx ui/src/pages/__tests__/ClassicDashboardPage.test.tsx ui/src/App.tsx ui/src/components/Sidebar.tsx
git commit -m "feat(ui): preserve existing widget-grid dashboard as Classic"
```

---

### Task 4: Build shared dashboard primitives

**Files:**
- Create: `ui/src/components/dashboard/BatteryRing.tsx`
- Create: `ui/src/components/dashboard/PowerFlowPill.tsx`
- Create: `ui/src/components/dashboard/SummaryBadge.tsx`
- Test: `ui/src/components/dashboard/__tests__/BatteryRing.test.tsx`

- [ ] **Step 1: Write a failing BatteryRing test**

Create `ui/src/components/dashboard/__tests__/BatteryRing.test.tsx`:

```tsx
import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import BatteryRing from '../BatteryRing'

describe('BatteryRing', () => {
  it('renders the SoC percentage', () => {
    render(<BatteryRing soc={73} status="charging" />)
    expect(screen.getByText('73%')).toBeInTheDocument()
  })

  it('renders zero when SoC is null', () => {
    render(<BatteryRing soc={null} status="unknown" />)
    expect(screen.getByText('0%')).toBeInTheDocument()
  })
})
```

Run it and expect it to fail.

- [ ] **Step 2: Create BatteryRing**

Create `ui/src/components/dashboard/BatteryRing.tsx`:

```tsx
interface BatteryRingProps {
  soc: number | null
  status: 'charging' | 'discharging' | 'balanced' | 'unknown'
  size?: 'sm' | 'md'
}

export default function BatteryRing({ soc, status, size = 'md' }: BatteryRingProps) {
  const pct = Math.min(100, Math.max(0, soc ?? 0))
  const radius = size === 'sm' ? 26 : 54
  const stroke = size === 'sm' ? 6 : 10
  const circumference = 2 * Math.PI * radius
  const offset = circumference - (pct / 100) * circumference

  const color =
    status === 'charging'
      ? 'text-emerald-500'
      : status === 'discharging'
      ? 'text-amber-500'
      : 'text-cyan-500'

  const box = size === 'sm' ? 'h-16 w-16' : 'h-40 w-40'
  const viewBox = size === 'sm' ? '0 0 64 64' : '0 0 120 120'
  const center = size === 'sm' ? 32 : 60
  const textMain = size === 'sm' ? 'text-sm' : 'text-3xl'
  const textSub = size === 'sm' ? 'text-[8px]' : 'text-xs'

  return (
    <div className="flex flex-col items-center">
      <div className={`relative ${box}`}>
        <svg className="h-full w-full -rotate-90" viewBox={viewBox}>
          <circle
            cx={center}
            cy={center}
            r={radius}
            className="stroke-slate-200"
            strokeWidth={stroke}
            fill="none"
          />
          <circle
            cx={center}
            cy={center}
            r={radius}
            className={`${color} transition-all duration-700`}
            strokeWidth={stroke}
            fill="none"
            strokeDasharray={circumference}
            strokeDashoffset={offset}
            strokeLinecap="round"
          />
        </svg>
        <div className="absolute inset-0 flex flex-col items-center justify-center">
          <span className={`font-bold text-slate-800 ${textMain}`}>{pct.toFixed(0)}%</span>
          <span className={`text-slate-500 capitalize ${textSub}`}>{status}</span>
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 3: Create PowerFlowPill**

Create `ui/src/components/dashboard/PowerFlowPill.tsx`:

```tsx
interface PowerFlowPillProps {
  pv: number
  battery: number
  load: number
}

export default function PowerFlowPill({ pv, battery, load }: PowerFlowPillProps) {
  const batteryIsCharging = battery > 0

  return (
    <div className="bg-white rounded-xl shadow-sm border border-slate-200 p-4">
      <div className="flex items-center justify-between text-center">
        <div className="flex-1">
          <div className="text-xs text-slate-400 uppercase tracking-wide">PV</div>
          <div className="text-lg font-semibold text-emerald-600">{pv.toFixed(0)} W</div>
        </div>
        <div className="text-slate-300 px-2">→</div>
        <div className="flex-1">
          <div className="text-xs text-slate-400 uppercase tracking-wide">Battery</div>
          <div className={`text-lg font-semibold ${batteryIsCharging ? 'text-emerald-600' : 'text-amber-600'}`}>
            {Math.abs(battery).toFixed(0)} W
          </div>
          <div className="text-[10px] text-slate-400">{batteryIsCharging ? 'Charging' : 'Discharging'}</div>
        </div>
        <div className="text-slate-300 px-2">→</div>
        <div className="flex-1">
          <div className="text-xs text-slate-400 uppercase tracking-wide">Load</div>
          <div className="text-lg font-semibold text-slate-700">{load.toFixed(0)} W</div>
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 4: Create SummaryBadge**

Create `ui/src/components/dashboard/SummaryBadge.tsx`:

```tsx
interface SummaryBadgeProps {
  label: string
  value: number | string | undefined | null
  unit?: string
  decimals?: number
  className?: string
}

export default function SummaryBadge({
  label,
  value,
  unit = '',
  decimals = 1,
  className = '',
}: SummaryBadgeProps) {
  const display =
    typeof value === 'number'
      ? value.toFixed(decimals)
      : value ?? '--'

  return (
    <div className={`bg-white rounded-lg shadow-sm border border-slate-200 p-3 ${className}`}>
      <div className="text-xs text-slate-500 uppercase tracking-wide">{label}</div>
      <div className="mt-1 text-lg font-semibold text-slate-800">
        {display}
        {unit && (
          <span className="text-sm font-normal text-slate-500 ml-0.5">{unit}</span>
        )}
      </div>
    </div>
  )
}
```

- [ ] **Step 5: Run the primitive tests and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/components/dashboard/__tests__/BatteryRing.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/components/dashboard/BatteryRing.tsx ui/src/components/dashboard/PowerFlowPill.tsx ui/src/components/dashboard/SummaryBadge.tsx ui/src/components/dashboard/__tests__/BatteryRing.test.tsx
git commit -m "feat(ui): add dashboard primitives: battery ring, power flow pill, summary badge"
```

---

### Task 5: Build the mobile inverter-app dashboard

**Files:**
- Create: `ui/src/components/dashboard/MobileChannelCard.tsx`
- Create: `ui/src/components/dashboard/RelayStatusPanel.tsx`
- Create: `ui/src/components/dashboard/MobileDashboard.tsx`
- Test: `ui/src/components/dashboard/__tests__/MobileDashboard.test.tsx`

- [ ] **Step 1: Write a failing MobileDashboard test**

Create `ui/src/components/dashboard/__tests__/MobileDashboard.test.tsx`:

```tsx
import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import { Provider, createStore } from 'jotai'
import { latestAtom } from '../../../state/atoms'
import MobileDashboard from '../MobileDashboard'

function makePoint(payload: Record<string, number>) {
  return {
    id: 1,
    device_id: 'k',
    recorded_at: new Date().toISOString(),
    payload,
    metadata: {},
  }
}

describe('MobileDashboard', () => {
  it('renders the battery ring, flow pill, and channel cards', () => {
    const store = createStore()
    store.set(latestAtom, makePoint({
      ch0_V: 12.3,
      ch0_I: 1.2,
      ch0_P: 14.8,
      soc_pct0: 78,
      energy_wh0: 1200,
    }))

    render(
      <Provider store={store}>
        <MobileDashboard />
      </Provider>
    )

    expect(screen.getByText('78%')).toBeInTheDocument()
    expect(screen.getByText('PV')).toBeInTheDocument()
    expect(screen.getByText('Battery')).toBeInTheDocument()
    expect(screen.getByText('Load')).toBeInTheDocument()
    expect(screen.getByText('Channel 1')).toBeInTheDocument()
    expect(screen.getByText('Channel 4')).toBeInTheDocument()
  })
})
```

Run it and expect it to fail because the mobile components do not exist.

- [ ] **Step 2: Create MobileChannelCard**

Create `ui/src/components/dashboard/MobileChannelCard.tsx`:

```tsx
import { useAtomValue } from 'jotai'
import { channelPayloadAtomFamily } from '../../state/derived'

interface MobileChannelCardProps {
  channel: number
}

export default function MobileChannelCard({ channel }: MobileChannelCardProps) {
  const data = useAtomValue(channelPayloadAtomFamily(channel))

  return (
    <div className="bg-white rounded-xl shadow-sm border border-slate-200 p-4 active:bg-slate-50 transition-colors">
      <div className="flex items-center justify-between">
        <span className="text-sm font-semibold text-slate-700">Channel {channel + 1}</span>
        {data.socPct != null && (
          <span className="text-xs font-medium text-emerald-700 bg-emerald-100 px-2 py-0.5 rounded-full">
            SoC {data.socPct.toFixed(0)}%
          </span>
        )}
      </div>
      <div className="mt-3 grid grid-cols-3 gap-2 text-center text-sm">
        <div>
          <div className="text-xs text-slate-400">V</div>
          <div className="font-semibold text-slate-800">{data.voltage?.toFixed(2) ?? '--'}</div>
        </div>
        <div>
          <div className="text-xs text-slate-400">A</div>
          <div className="font-semibold text-slate-800">{data.current?.toFixed(2) ?? '--'}</div>
        </div>
        <div>
          <div className="text-xs text-slate-400">W</div>
          <div className="font-semibold text-slate-800">{data.power?.toFixed(1) ?? '--'}</div>
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 3: Create RelayStatusPanel**

Create `ui/src/components/dashboard/RelayStatusPanel.tsx`:

```tsx
import type { RelayState } from '../../lib/types'

interface RelayStatusPanelProps {
  relays: RelayState[]
  compact?: boolean
}

export default function RelayStatusPanel({ relays, compact }: RelayStatusPanelProps) {
  if (relays.length === 0) return null

  return (
    <div className={`bg-white rounded-xl shadow-sm border border-slate-200 ${compact ? 'p-3' : 'p-5'}`}>
      <h3 className={`font-semibold text-slate-700 ${compact ? 'text-xs mb-2' : 'text-sm mb-3'}`}>
        Relays
      </h3>
      <div className={`grid gap-2 ${compact ? 'grid-cols-2' : 'grid-cols-1'}`}>
        {relays.map((r) => (
          <div key={r.id} className="flex items-center justify-between text-sm">
            <span className="text-slate-600">Relay {r.relay_index + 1}</span>
            <span
              className={`px-2 py-0.5 rounded text-xs font-medium ${
                r.is_energized
                  ? 'bg-emerald-100 text-emerald-700'
                  : 'bg-slate-100 text-slate-600'
              }`}
            >
              {r.is_energized ? 'ON' : 'OFF'}
            </span>
          </div>
        ))}
      </div>
    </div>
  )
}
```

- [ ] **Step 4: Create MobileDashboard**

Create `ui/src/components/dashboard/MobileDashboard.tsx`:

```tsx
import { useAtomValue } from 'jotai'
import { computedTelemetryAtom } from '../../state/derived'
import { selectedDeviceAtom, relayStatesAtomFamily } from '../../state/atoms'
import BatteryRing from './BatteryRing'
import PowerFlowPill from './PowerFlowPill'
import SummaryBadge from './SummaryBadge'
import MobileChannelCard from './MobileChannelCard'
import RelayStatusPanel from './RelayStatusPanel'

const CHANNELS = [0, 1, 2, 3]

export default function MobileDashboard() {
  const telemetry = useAtomValue(computedTelemetryAtom)
  const device = useAtomValue(selectedDeviceAtom)
  const relays = useAtomValue(relayStatesAtomFamily(device?.device_key ?? ''))

  return (
    <div className="space-y-4 pb-6">
      <div className="flex justify-center py-3">
        <BatteryRing soc={telemetry.min_soc_pct} status={telemetry.system_status} />
      </div>

      <PowerFlowPill
        pv={telemetry.pv_power}
        battery={telemetry.battery_power}
        load={telemetry.dc_load_power}
      />

      <div className="grid grid-cols-2 gap-3">
        <SummaryBadge
          label="Today's Yield"
          value={telemetry.total_energy_wh / 1000}
          unit="kWh"
          decimals={2}
        />
        <SummaryBadge
          label="Status"
          value={telemetry.system_status}
          decimals={0}
        />
      </div>

      <div className="space-y-3">
        {CHANNELS.map((ch) => (
          <MobileChannelCard key={ch} channel={ch} />
        ))}
      </div>

      <RelayStatusPanel relays={relays} compact />
    </div>
  )
}
```

- [ ] **Step 5: Run the MobileDashboard test and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/components/dashboard/__tests__/MobileDashboard.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/components/dashboard/MobileChannelCard.tsx ui/src/components/dashboard/RelayStatusPanel.tsx ui/src/components/dashboard/MobileDashboard.tsx ui/src/components/dashboard/__tests__/MobileDashboard.test.tsx
git commit -m "feat(ui): add mobile inverter-app dashboard view"
```

---

### Task 6: Build desktop detail components

**Files:**
- Create: `ui/src/components/dashboard/DesktopSummaryCard.tsx`
- Create: `ui/src/components/dashboard/DesktopChannelCard.tsx`
- Create: `ui/src/components/dashboard/BatteryDetailPanel.tsx`
- Test: `ui/src/components/dashboard/__tests__/DesktopChannelCard.test.tsx`

- [ ] **Step 1: Write a failing DesktopChannelCard test**

Create `ui/src/components/dashboard/__tests__/DesktopChannelCard.test.tsx`:

```tsx
import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import { Provider, createStore } from 'jotai'
import { latestAtom } from '../../../state/atoms'
import DesktopChannelCard from '../DesktopChannelCard'

function makePoint(payload: Record<string, number>) {
  return {
    id: 1,
    device_id: 'k',
    recorded_at: new Date().toISOString(),
    payload,
    metadata: {},
  }
}

describe('DesktopChannelCard', () => {
  it('renders all channel metrics', () => {
    const store = createStore()
    store.set(latestAtom, makePoint({
      ch1_V: 24.5,
      ch1_I: 3.1,
      ch1_P: 76.0,
      soc_pct1: 82,
      energy_wh1: 5400,
    }))

    render(
      <Provider store={store}>
        <DesktopChannelCard channel={1} />
      </Provider>
    )

    expect(screen.getByText('Channel 2')).toBeInTheDocument()
    expect(screen.getByText('24.50')).toBeInTheDocument()
    expect(screen.getByText('3.10')).toBeInTheDocument()
    expect(screen.getByText('76.0')).toBeInTheDocument()
    expect(screen.getByText('SoC 82.0%')).toBeInTheDocument()
  })
})
```

Run it and expect it to fail.

- [ ] **Step 2: Create DesktopSummaryCard**

Create `ui/src/components/dashboard/DesktopSummaryCard.tsx`:

```tsx
import type { ComponentType } from 'react'

interface DesktopSummaryCardProps {
  label: string
  value: number | string | undefined | null
  unit?: string
  decimals?: number
  icon: ComponentType<{ className?: string }>
  color: 'emerald' | 'amber' | 'cyan' | 'blue' | 'indigo' | 'slate'
}

const colorMap: Record<string, string> = {
  emerald: 'text-emerald-600 bg-emerald-50',
  amber: 'text-amber-600 bg-amber-50',
  cyan: 'text-cyan-600 bg-cyan-50',
  blue: 'text-blue-600 bg-blue-50',
  indigo: 'text-indigo-600 bg-indigo-50',
  slate: 'text-slate-600 bg-slate-50',
}

export default function DesktopSummaryCard({
  label,
  value,
  unit = '',
  decimals = 1,
  icon: Icon,
  color,
}: DesktopSummaryCardProps) {
  const display =
    typeof value === 'number'
      ? value.toFixed(decimals)
      : value ?? '--'

  return (
    <div className="bg-white rounded-xl shadow-sm border border-slate-200 p-4 flex items-center gap-4">
      <div className={`h-12 w-12 rounded-full flex items-center justify-center ${colorMap[color]}`}>
        <Icon className="h-6 w-6" />
      </div>
      <div className="min-w-0">
        <div className="text-xs text-slate-500 uppercase tracking-wide">{label}</div>
        <div className="text-xl font-bold text-slate-800 truncate">
          {display}
          {unit && <span className="text-sm font-normal text-slate-500 ml-0.5">{unit}</span>}
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 3: Create DesktopChannelCard**

Create `ui/src/components/dashboard/DesktopChannelCard.tsx`:

```tsx
import { useAtomValue } from 'jotai'
import { channelPayloadAtomFamily } from '../../state/derived'

interface DesktopChannelCardProps {
  channel: number
}

export default function DesktopChannelCard({ channel }: DesktopChannelCardProps) {
  const data = useAtomValue(channelPayloadAtomFamily(channel))

  return (
    <div className="bg-white rounded-xl shadow-sm border border-slate-200 p-5">
      <div className="flex items-center justify-between mb-4">
        <span className="font-semibold text-slate-800">Channel {channel + 1}</span>
        {data.socPct != null && (
          <span className="text-xs font-medium text-emerald-700 bg-emerald-100 px-2 py-1 rounded-full">
            SoC {data.socPct.toFixed(1)}%
          </span>
        )}
      </div>
      <div className="grid grid-cols-3 gap-4">
        <div>
          <div className="text-xs text-slate-400 uppercase">Voltage</div>
          <div className="text-xl font-semibold text-slate-800">
            {data.voltage?.toFixed(2) ?? '--'}
            <span className="text-sm font-normal text-slate-500"> V</span>
          </div>
        </div>
        <div>
          <div className="text-xs text-slate-400 uppercase">Current</div>
          <div className="text-xl font-semibold text-slate-800">
            {data.current?.toFixed(2) ?? '--'}
            <span className="text-sm font-normal text-slate-500"> A</span>
          </div>
        </div>
        <div>
          <div className="text-xs text-slate-400 uppercase">Power</div>
          <div className="text-xl font-semibold text-slate-800">
            {data.power?.toFixed(1) ?? '--'}
            <span className="text-sm font-normal text-slate-500"> W</span>
          </div>
        </div>
      </div>
      {data.energyWh != null && (
        <div className="mt-4 pt-3 border-t border-slate-100 text-xs text-slate-500">
          Energy today: {(data.energyWh / 1000).toFixed(2)} kWh
        </div>
      )}
    </div>
  )
}
```

- [ ] **Step 4: Create BatteryDetailPanel**

Create `ui/src/components/dashboard/BatteryDetailPanel.tsx`:

```tsx
import BatteryRing from './BatteryRing'

interface BatteryDetailPanelProps {
  soc: number | null
  status: 'charging' | 'discharging' | 'balanced' | 'unknown'
}

export default function BatteryDetailPanel({ soc, status }: BatteryDetailPanelProps) {
  const pct = Math.min(100, Math.max(0, soc ?? 0))

  return (
    <div className="bg-white rounded-xl shadow-sm border border-slate-200 p-5">
      <h3 className="text-sm font-semibold text-slate-700 mb-3">Battery</h3>
      <div className="flex items-center gap-4">
        <BatteryRing soc={soc} status={status} size="sm" />
        <div>
          <div className="text-2xl font-bold text-slate-800">{pct.toFixed(0)}%</div>
          <div className="text-xs text-slate-500 capitalize">{status}</div>
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 5: Run the desktop component tests and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/components/dashboard/__tests__/DesktopChannelCard.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/components/dashboard/DesktopSummaryCard.tsx ui/src/components/dashboard/DesktopChannelCard.tsx ui/src/components/dashboard/BatteryDetailPanel.tsx ui/src/components/dashboard/__tests__/DesktopChannelCard.test.tsx
git commit -m "feat(ui): add desktop detail dashboard components"
```

---

### Task 7: Assemble the multi-column desktop dashboard

**Files:**
- Create: `ui/src/components/dashboard/DesktopDashboard.tsx`
- Test: `ui/src/components/dashboard/__tests__/DesktopDashboard.test.tsx`

- [ ] **Step 1: Write a failing DesktopDashboard test**

Create `ui/src/components/dashboard/__tests__/DesktopDashboard.test.tsx`:

```tsx
import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import { Provider, createStore } from 'jotai'
import { latestAtom } from '../../../state/atoms'
import DesktopDashboard from '../DesktopDashboard'

function makePoint(payload: Record<string, number>) {
  return {
    id: 1,
    device_id: 'k',
    recorded_at: new Date().toISOString(),
    payload,
    metadata: {},
  }
}

describe('DesktopDashboard', () => {
  it('renders the summary row, channel cards, and battery panel', () => {
    const store = createStore()
    store.set(latestAtom, makePoint({
      ch0_V: 12.3,
      ch0_I: 1.2,
      ch0_P: 14.8,
      soc_pct0: 88,
      energy_wh0: 1200,
    }))

    render(
      <Provider store={store}>
        <DesktopDashboard />
      </Provider>
    )

    expect(screen.getByText('PV Power')).toBeInTheDocument()
    expect(screen.getByText('Load')).toBeInTheDocument()
    expect(screen.getByText('Battery')).toBeInTheDocument()
    expect(screen.getByText('Channel 1')).toBeInTheDocument()
    expect(screen.getByText('Channel 4')).toBeInTheDocument()
  })
})
```

Run it and expect it to fail.

- [ ] **Step 2: Create DesktopDashboard**

Create `ui/src/components/dashboard/DesktopDashboard.tsx`:

```tsx
import { useAtomValue } from 'jotai'
import {
  SunIcon,
  BoltIcon,
  Battery50Icon,
  ChartPieIcon,
  ArrowTrendingUpIcon,
  Cog6ToothIcon,
} from '@heroicons/react/24/outline'
import { computedTelemetryAtom } from '../../state/derived'
import { selectedDeviceAtom, relayStatesAtomFamily } from '../../state/atoms'
import DesktopSummaryCard from './DesktopSummaryCard'
import DesktopChannelCard from './DesktopChannelCard'
import BatteryDetailPanel from './BatteryDetailPanel'
import RelayStatusPanel from './RelayStatusPanel'

const CHANNELS = [0, 1, 2, 3]

export default function DesktopDashboard() {
  const telemetry = useAtomValue(computedTelemetryAtom)
  const device = useAtomValue(selectedDeviceAtom)
  const relays = useAtomValue(relayStatesAtomFamily(device?.device_key ?? ''))

  return (
    <div className="space-y-6">
      {/* Top summary row — 2 cols on small, 3 on medium, 6 on large */}
      <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-6 gap-4">
        <DesktopSummaryCard
          label="PV Power"
          value={telemetry.pv_power}
          unit="W"
          icon={SunIcon}
          color="emerald"
        />
        <DesktopSummaryCard
          label="Battery Power"
          value={telemetry.battery_power}
          unit="W"
          icon={Battery50Icon}
          color={telemetry.battery_power >= 0 ? 'emerald' : 'amber'}
        />
        <DesktopSummaryCard
          label="Load"
          value={telemetry.dc_load_power}
          unit="W"
          icon={BoltIcon}
          color="slate"
        />
        <DesktopSummaryCard
          label="SoC"
          value={telemetry.min_soc_pct}
          unit="%"
          decimals={0}
          icon={ChartPieIcon}
          color="cyan"
        />
        <DesktopSummaryCard
          label="Today's Yield"
          value={telemetry.total_energy_wh / 1000}
          unit="kWh"
          decimals={2}
          icon={ArrowTrendingUpIcon}
          color="blue"
        />
        <DesktopSummaryCard
          label="System Status"
          value={telemetry.system_status}
          decimals={0}
          icon={Cog6ToothIcon}
          color="indigo"
        />
      </div>

      {/* Main content: channel grid + side panels */}
      <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
        <div className="lg:col-span-2 grid grid-cols-1 md:grid-cols-2 gap-4">
          {CHANNELS.map((ch) => (
            <DesktopChannelCard key={ch} channel={ch} />
          ))}
        </div>

        <div className="space-y-4">
          <BatteryDetailPanel
            soc={telemetry.min_soc_pct}
            status={telemetry.system_status}
          />
          <RelayStatusPanel relays={relays} />
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 3: Run the DesktopDashboard test and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/components/dashboard/__tests__/DesktopDashboard.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 4: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/components/dashboard/DesktopDashboard.tsx ui/src/components/dashboard/__tests__/DesktopDashboard.test.tsx
git commit -m "feat(ui): add multi-column desktop dashboard"
```

---

### Task 8: Rewrite DashboardPage to switch views by breakpoint

**Files:**
- Modify: `ui/src/pages/DashboardPage.tsx` (full rewrite)
- Test: `ui/src/pages/__tests__/DashboardPage.test.tsx`

- [ ] **Step 1: Write a failing DashboardPage test**

Create `ui/src/pages/__tests__/DashboardPage.test.tsx`:

```tsx
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import { Provider } from 'jotai'
import DashboardPage from '../DashboardPage'

function mockMatchMedia(matches: boolean) {
  return vi.fn().mockImplementation((query: string) => ({
    matches,
    media: query,
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    dispatchEvent: vi.fn(),
  }))
}

vi.mock('../../lib/supabase', () => ({
  supabase: {
    auth: {
      getSession: vi.fn().mockResolvedValue({ data: { session: null } }),
      signOut: vi.fn(),
    },
    from: vi.fn().mockReturnValue({
      select: vi.fn().mockReturnValue({ order: vi.fn().mockResolvedValue({ data: [] }) }),
    }),
  },
}))

describe('DashboardPage responsive switch', () => {
  beforeEach(() => {
    vi.stubGlobal('matchMedia', mockMatchMedia(false))
  })

  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('renders the loading state while auth is loading', async () => {
    render(
      <BrowserRouter>
        <Provider>
          <DashboardPage />
        </Provider>
      </BrowserRouter>
    )
    expect(await screen.findByText('Loading...')).toBeInTheDocument()
  })
})
```

Run it and expect it to fail because DashboardPage has not been rewritten yet.

- [ ] **Step 2: Rewrite DashboardPage.tsx**

Replace the entire contents of `ui/src/pages/DashboardPage.tsx` with:

```tsx
import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useStore } from 'jotai'
import { selectedDeviceAtom } from '../state/atoms'
import { supabase } from '../lib/supabase'
import { loadChannels } from '../state/services/channelsService'
import type { Device } from '../lib/types'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'
import MobileDashboard from '../components/dashboard/MobileDashboard'
import DesktopDashboard from '../components/dashboard/DesktopDashboard'
import { useIsMobile } from '../lib/useIsMobile'
import { APP_VERSION } from '../lib/version'

export default function DashboardPage() {
  const navigate = useNavigate()
  const store = useStore()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const [userId, setUserId] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const isMobile = useIsMobile()

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
    Promise.all([
      startLiveTelemetrySafe(store, selectedDevice.device_key),
      loadChannels(store, selectedDevice.device_key),
      startAggregatesPollingSafe(store),
    ])
    return () => {
      stopLiveTelemetrySafe()
      stopAggregatesPollingSafe()
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
      version={APP_VERSION}
    >
      {!selectedDevice ? (
        <div className="text-center py-12">
          <p className="text-slate-600 mb-4">No device selected.</p>
          <p className="text-sm text-slate-400">Select a device from the dropdown above to view telemetry.</p>
        </div>
      ) : (
        isMobile ? <MobileDashboard /> : <DesktopDashboard />
      )}
    </DashboardLayout>
  )
}

async function startLiveTelemetrySafe(store: any, deviceKey: string) {
  const { startLiveTelemetry } = await import('../state/services/telemetryService')
  startLiveTelemetry(store, deviceKey)
}

async function stopLiveTelemetrySafe() {
  const { stopLiveTelemetry } = await import('../state/services/telemetryService')
  stopLiveTelemetry()
}

async function startAggregatesPollingSafe(store: any) {
  const { startAggregatesPolling } = await import('../state/services/aggregatesService')
  startAggregatesPolling(store)
}

async function stopAggregatesPollingSafe() {
  const { stopAggregatesPolling } = await import('../state/services/aggregatesService')
  stopAggregatesPolling()
}
```

Note: the new dashboard does **not** load the user layout from `layoutService`; the classic page still does. This is intentional because the new dashboard has its own fixed responsive layout.

- [ ] **Step 3: Run the DashboardPage test and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/pages/__tests__/DashboardPage.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 4: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/pages/DashboardPage.tsx ui/src/pages/__tests__/DashboardPage.test.tsx
git commit -m "feat(ui): make DashboardPage responsive with mobile/desktop views"
```

---

### Task 9: Display the app version in the sidebar

**Files:**
- Modify: `ui/src/components/Sidebar.tsx:9-16`, `ui/src/components/Sidebar.tsx:45-52`, `ui/src/components/Sidebar.tsx:76-85`
- Modify: `ui/src/components/DashboardLayout.tsx:4-11`, `ui/src/components/DashboardLayout.tsx:28-35`
- Modify: `ui/src/pages/ClassicDashboardPage.tsx` to pass version
- Test: `ui/src/components/__tests__/Sidebar.test.tsx`

- [ ] **Step 1: Write the failing Sidebar test**

Create `ui/src/components/__tests__/Sidebar.test.tsx`:

```tsx
import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import Sidebar from '../Sidebar'

const noop = vi.fn()

describe('Sidebar', () => {
  it('renders the version badge', () => {
    render(
      <BrowserRouter>
        <Sidebar
          currentPath="/dashboard"
          onNavigate={noop}
          onSignOut={noop}
          isOpen={true}
          onClose={noop}
          deviceName="Test Device"
          version="0.2.0"
        />
      </BrowserRouter>
    )

    expect(screen.getByText('v0.2.0')).toBeInTheDocument()
  })

  it('renders the Dashboard and Classic Dashboard nav items', () => {
    render(
      <BrowserRouter>
        <Sidebar
          currentPath="/dashboard"
          onNavigate={noop}
          onSignOut={noop}
          isOpen={true}
          onClose={noop}
          version="0.2.0"
        />
      </BrowserRouter>
    )

    expect(screen.getByRole('button', { name: /Dashboard$/ })).toBeInTheDocument()
    expect(screen.getByRole('button', { name: 'Classic Dashboard' })).toBeInTheDocument()
  })
})
```

Run it and expect it to fail because `version` prop and badge do not exist.

- [ ] **Step 2: Add the version prop and badge to Sidebar**

In `ui/src/components/Sidebar.tsx`, update the interface:

```ts
export interface SidebarProps {
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  isOpen: boolean
  onClose: () => void
  deviceName?: string
  version?: string
}
```

Update the default parameters:

```ts
export default function Sidebar({
  currentPath,
  onNavigate,
  onSignOut,
  isOpen,
  onClose,
  deviceName = 'IoT Dashboard',
  version,
}: SidebarProps) {
```

Update the header block (around lines 47-52) to:

```tsx
<div className="px-6 py-5 border-b border-slate-700/50">
  <div className="text-lg font-semibold text-white tracking-tight">
    {deviceName}
  </div>
  <div className="flex items-center gap-2 mt-0.5">
    <div className="text-xs text-slate-400">Power Monitor</div>
    {version && (
      <span className="text-[10px] leading-none font-medium px-1.5 py-0.5 rounded bg-cyan-500/20 text-cyan-300 border border-cyan-500/30">
        v{version}
      </span>
    )}
  </div>
</div>
```

- [ ] **Step 3: Thread the version through DashboardLayout**

Update `DashboardLayoutProps` in `ui/src/components/DashboardLayout.tsx`:

```ts
export interface DashboardLayoutProps {
  children: ReactNode
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  header: ReactNode | ((props: { onMenuClick: () => void }) => ReactNode)
  deviceName?: string
  version?: string
}
```

Update the destructuring and Sidebar call:

```ts
export default function DashboardLayout({
  children,
  currentPath,
  onNavigate,
  onSignOut,
  header,
  deviceName,
  version,
}: DashboardLayoutProps) {
```

```tsx
<Sidebar
  currentPath={currentPath}
  onNavigate={(path) => { onNavigate(path); setSidebarOpen(false) }}
  onSignOut={onSignOut}
  isOpen={sidebarOpen}
  onClose={() => setSidebarOpen(false)}
  deviceName={deviceName}
  version={version}
/>
```

- [ ] **Step 4: Pass the version from the classic page**

In `ui/src/pages/ClassicDashboardPage.tsx`, add the import:

```ts
import { APP_VERSION } from '../lib/version'
```

And add `version={APP_VERSION}` to its `DashboardLayout` props.

- [ ] **Step 5: Run the Sidebar test and build**

Run:
```bash
cd /home/sayem/sources/power-monitoring/ui
npm test -- src/components/__tests__/Sidebar.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
cd /home/sayem/sources/power-monitoring
git add ui/src/components/Sidebar.tsx ui/src/components/DashboardLayout.tsx ui/src/pages/ClassicDashboardPage.tsx ui/src/components/__tests__/Sidebar.test.tsx
git commit -m "feat(ui): show app version badge in sidebar menu"
```

---

### Task 10: Run the full UI test suite and production build

**Files:**
- No new files.

- [ ] **Step 1: Run all UI tests**

```bash
cd /home/sayem/sources/power-monitoring/ui
npm test
```

Expected: All tests pass.

- [ ] **Step 2: Run the production build**

```bash
cd /home/sayem/sources/power-monitoring/ui
npm run build
```

Expected: `dist/` is generated with no TypeScript or Vite errors.

- [ ] **Step 3: Smoke test the dev server (optional but recommended)**

```bash
cd /home/sayem/sources/power-monitoring/ui
npm run dev &
```

Open `http://localhost:3000` in a browser, sign in, and verify:
- The sidebar shows `v0.2.0` next to "Power Monitor".
- The sidebar has both "Dashboard" and "Classic Dashboard".
- Clicking "Dashboard" renders the new view.
- On a narrow viewport, the new dashboard shows a large battery ring, a PV → Battery → Load pill, and stacked channel cards (inverter-app feel).
- On a wide viewport, the new dashboard shows a six-card summary row, a two-column channel grid, and a battery/relay side panel.
- Clicking "Classic Dashboard" renders the old widget grid.

Stop the dev server when done.

- [ ] **Step 4: Commit any final fixes**

If no fixes were needed, no extra commit is required. If you made changes, commit them with a clear message, e.g.:

```bash
git commit -am "fix(ui): address responsive dashboard review feedback"
```

---

## Self-Review

### 1. Spec coverage

| Requirement | Task |
|-------------|------|
| Create a new dashboard | Tasks 4-8 |
| Mobile view looks like a power-station / inverter app | Task 5: big battery ring, PV → Battery → Load flow pill, thumb-friendly cards, status badges |
| Desktop view is detailed, feature-rich, multi-column | Tasks 6-7: six summary cards, detailed channel grid, battery panel, relay panel |
| Both views are user-friendly | Large tap targets on mobile, clear labels/icons on desktop, consistent color-coded status, no clutter |
| Keep existing dashboard as "Classic" | Task 3 (`ClassicDashboardPage` + `/dashboard/classic` route + sidebar label) |
| Add version to distinguish what version we are seeing | Task 1 (version constant), Task 9 (badge in sidebar) |
| Menu is a nice place for the version | Task 9 (badge sits next to "Power Monitor" in the sidebar header) |

### 2. Placeholder scan

No placeholders such as "TBD", "implement later", or "write tests for the above" remain. Every step contains exact file paths, exact code blocks, exact commands, and expected output.

### 3. Type consistency

- `APP_VERSION` is a string and is passed as `version?: string` into `SidebarProps` and `DashboardLayoutProps`.
- `useIsMobile()` returns `boolean` and is used in `DashboardPage` to choose between `<MobileDashboard />` and `<DesktopDashboard />`.
- `BatteryRing` accepts `size?: 'sm' | 'md'` and is reused inside `BatteryDetailPanel`.
- `channelPayloadAtomFamily` is used the same way in `MobileChannelCard`, `DesktopChannelCard`, and the existing derived state tests.
- `DashboardLayout` continues to accept the same `header` render prop and children API.
- Heroicons used (`SunIcon`, `BoltIcon`, `Battery50Icon`, `ChartPieIcon`, `ArrowTrendingUpIcon`, `Cog6ToothIcon`) are all from `@heroicons/react/24/outline`, which is already a dependency.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-13-new-responsive-dashboard.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. **REQUIRED SUB-SKILL:** `superpowers:subagent-driven-development`.

**2. Inline Execution** — Execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints for review.

**Which approach?**
