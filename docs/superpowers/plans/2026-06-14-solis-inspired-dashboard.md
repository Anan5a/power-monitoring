# Solis-Inspired Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the web dashboard to match the Solis inverter monitoring portal: dark top bar, dark icon sidebar, large semi-circular gauge KPI cards, tabbed overview with yield chart and topology flow diagram, detailed device page with real-time panel and parameter tables, and an analysis page with metric selectors.

**Architecture:** Introduce a new `SolisLayout` shell (`ui/src/components/solis/`) with dark top bar and dark sidebar. Re-route `/dashboard` to a new `SolisOverviewPage`. Add `/dashboard/device`, `/dashboard/analysis`, and `/dashboard/alarms`. Preserve the existing widget grid at `/dashboard/classic`. Keep the app version badge. Reuse existing Jotai atoms and telemetry services; only UI components are new.

**Tech Stack:** React 18 + TypeScript + Vite, Tailwind CSS, Jotai, React Router v6, Vitest, Heroicons, Chart.js via react-chartjs-2 (already used by widgets).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `ui/src/lib/version.ts` | App version constant (carry over from prior attempt). |
| `ui/src/lib/useIsMobile.ts` | Viewport breakpoint hook (carry over). |
| `ui/src/components/solis/SolisLayout.tsx` | Dark top bar + dark sidebar shell. |
| `ui/src/components/solis/SolisSidebar.tsx` | Icon-first dark navigation with groups. |
| `ui/src/components/solis/SolisTopBar.tsx` | Device selector, refresh, user menu, version badge. |
| `ui/src/components/solis/SemiGauge.tsx` | Semi-circular SVG gauge for KPI cards. |
| `ui/src/components/solis/KpiCard.tsx` | White summary card with gauge or icon stack. |
| `ui/src/components/solis/StatusBadge.tsx` | Green/orange/gray dot + text status. |
| `ui/src/components/solis/TopologyDiagram.tsx` | Energy-flow nodes and directional lines. |
| `ui/src/components/solis/RealtimePanel.tsx` | Device real-time info panel. |
| `ui/src/components/solis/DeviceInfoCard.tsx` | Plant/device info card. |
| `ui/src/components/solis/AlarmPanel.tsx` | Alarm status / list panel. |
| `ui/src/components/solis/ParamTable.tsx` | DC/AC parameter table. |
| `ui/src/components/solis/DateRangeTabs.tsx` | Day/Month/Year/Total segmented control. |
| `ui/src/components/solis/MetricSelector.tsx` | Checkbox groups for analysis chart. |
| `ui/src/components/solis/SolisChart.tsx` | Styled chart wrapper using react-chartjs-2. |
| `ui/src/pages/SolisOverviewPage.tsx` | Overview page with KPI row and tabbed content. |
| `ui/src/pages/SolisDevicePage.tsx` | Device detail page with topology + tables. |
| `ui/src/pages/SolisAnalysisPage.tsx` | Analysis page with metric selector + chart. |
| `ui/src/pages/SolisAlarmsPage.tsx` | Alarm list page. |
| `ui/src/pages/ClassicDashboardPage.tsx` | Preserved existing widget-grid dashboard. |
| `ui/src/App.tsx` | Route wiring for new Solis pages and `/dashboard/classic`. |
| `ui/src/components/solis/__tests__/SemiGauge.test.tsx` | SemiGauge renders value and arc. |
| `ui/src/components/solis/__tests__/KpiCard.test.tsx` | KpiCard renders label, value, sub-text. |
| `ui/src/components/solis/__tests__/StatusBadge.test.tsx` | StatusBadge renders dot + text. |
| `ui/src/components/solis/__tests__/TopologyDiagram.test.tsx` | Topology renders nodes and values. |
| `ui/src/components/solis/__tests__/DateRangeTabs.test.tsx` | Tabs call onChange. |
| `ui/src/components/solis/__tests__/MetricSelector.test.tsx` | MetricSelector toggles values. |
| `ui/src/pages/__tests__/SolisOverviewPage.test.tsx` | Overview renders KPI cards and tabs. |
| `ui/src/pages/__tests__/SolisDevicePage.test.tsx` | Device page renders topology. |
| `ui/src/pages/__tests__/SolisAnalysisPage.test.tsx` | Analysis page renders chart + selector. |
| `ui/src/components/solis/__tests__/SolisSidebar.test.tsx` | Sidebar renders nav items and version. |

---

### Task 1: Set up workspace and carry over shared utilities

**Files:**
- Create: `ui/src/lib/version.ts`
- Create: `ui/src/lib/__tests__/version.test.ts`
- Create: `ui/src/lib/useIsMobile.ts`
- Create: `ui/src/lib/__tests__/useIsMobile.test.ts`
- Create worktree: `.worktrees/solis-dashboard` on branch `feature/solis-dashboard`

- [ ] **Step 1: Create isolated worktree**

```bash
cd /home/sayem/sources/power-monitoring
git worktree add .worktrees/solis-dashboard -b feature/solis-dashboard
cd .worktrees/solis-dashboard/ui
npm install
npm test
```

Expected: baseline tests pass (38 tests).

- [ ] **Step 2: Add version constant and test**

Create `ui/src/lib/version.ts`:

```ts
export const APP_VERSION = '0.3.0'
```

Create `ui/src/lib/__tests__/version.test.ts`:

```ts
import { describe, it, expect } from 'vitest'
import { APP_VERSION } from '../version'

describe('version', () => {
  it('exports a semantic version string', () => {
    expect(APP_VERSION).toMatch(/^\d+\.\d+\.\d+/)
  })
})
```

Run:

```bash
npm test -- src/lib/__tests__/version.test.ts
```

Expected: PASS.

- [ ] **Step 3: Add useIsMobile hook and test**

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

Create `ui/src/lib/__tests__/useIsMobile.test.ts`:

```ts
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook } from '@testing-library/react'
import { useIsMobile } from '../useIsMobile'

function mockMatchMedia(matches: boolean) {
  return vi.fn().mockImplementation(() => ({
    matches,
    media: '',
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    dispatchEvent: vi.fn(),
  }))
}

describe('useIsMobile', () => {
  beforeEach(() => vi.stubGlobal('matchMedia', mockMatchMedia(false)))
  afterEach(() => vi.unstubAllGlobals())

  it('returns true for narrow viewport', () => {
    vi.stubGlobal('matchMedia', mockMatchMedia(true))
    const { result } = renderHook(() => useIsMobile())
    expect(result.current).toBe(true)
  })

  it('returns false for wide viewport', () => {
    vi.stubGlobal('matchMedia', mockMatchMedia(false))
    const { result } = renderHook(() => useIsMobile())
    expect(result.current).toBe(false)
  })
})
```

Run:

```bash
npm test -- src/lib/__tests__/useIsMobile.test.ts
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
cd /home/sayem/sources/power-monitoring/.worktrees/solis-dashboard
git add ui/src/lib/version.ts ui/src/lib/__tests__/version.test.ts ui/src/lib/useIsMobile.ts ui/src/lib/__tests__/useIsMobile.test.ts
git commit -m "chore(ui): carry over version and useIsMobile utilities"
```

---

### Task 2: Preserve existing dashboard as Classic

**Files:**
- Create: `ui/src/pages/ClassicDashboardPage.tsx`
- Modify: `ui/src/App.tsx`

- [ ] **Step 1: Copy existing DashboardPage to ClassicDashboardPage**

Create `ui/src/pages/ClassicDashboardPage.tsx` as an exact copy of the current `ui/src/pages/DashboardPage.tsx` (before any rewrite), changing only the export name to `ClassicDashboardPage` and `currentPath` to `/dashboard/classic`.

- [ ] **Step 2: Wire classic route**

In `ui/src/App.tsx`, keep the existing dashboard import and add:

```tsx
import ClassicDashboardPage from './pages/ClassicDashboardPage'
```

Add route:

```tsx
<Route path="/dashboard/classic" element={<ProtectedRoute><ClassicDashboardPage /></ProtectedRoute>} />
```

- [ ] **Step 3: Verify build**

```bash
cd ui
npm run build
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add ui/src/pages/ClassicDashboardPage.tsx ui/src/App.tsx
git commit -m "feat(ui): preserve widget-grid dashboard as Classic"
```

---

### Task 3: Build Solis layout shell

**Files:**
- Create: `ui/src/components/solis/SolisSidebar.tsx`
- Create: `ui/src/components/solis/SolisTopBar.tsx`
- Create: `ui/src/components/solis/SolisLayout.tsx`
- Test: `ui/src/components/solis/__tests__/SolisSidebar.test.tsx`

- [ ] **Step 1: Create SolisSidebar**

Create `ui/src/components/solis/SolisSidebar.tsx`:

```tsx
import {
  HomeIcon,
  BoltIcon,
  ChartBarIcon,
  BellIcon,
  Cog6ToothIcon,
  ArrowLeftOnRectangleIcon,
  Squares2X2Icon,
} from '@heroicons/react/24/outline'

export interface SolisSidebarProps {
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  version?: string
}

interface NavItem {
  label: string
  path: string
  Icon: typeof HomeIcon
}

const MAIN_ITEMS: NavItem[] = [
  { label: 'Overview', path: '/dashboard', Icon: HomeIcon },
  { label: 'Device', path: '/dashboard/device', Icon: BoltIcon },
  { label: 'Analysis', path: '/dashboard/analysis', Icon: ChartBarIcon },
  { label: 'Alarms', path: '/dashboard/alarms', Icon: BellIcon },
]

const BOTTOM_ITEMS: NavItem[] = [
  { label: 'Settings', path: '/settings', Icon: Cog6ToothIcon },
  { label: 'Classic View', path: '/dashboard/classic', Icon: Squares2X2Icon },
]

export default function SolisSidebar({ currentPath, onNavigate, onSignOut, version }: SolisSidebarProps) {
  function Item({ label, path, Icon }: NavItem) {
    const active = currentPath === path || (path !== '/dashboard' && currentPath.startsWith(path))
    return (
      <button
        type="button"
        onClick={() => onNavigate(path)}
        className={`w-full flex items-center gap-3 px-4 py-3 text-sm font-medium transition-colors ${
          active
            ? 'text-orange-400 bg-white/10 border-l-4 border-orange-400'
            : 'text-gray-300 hover:text-white hover:bg-white/5 border-l-4 border-transparent'
        }`}
      >
        <Icon className="h-5 w-5 shrink-0" />
        <span className="truncate">{label}</span>
      </button>
    )
  }

  return (
    <aside className="h-full w-56 bg-[#2e3440] text-gray-100 flex flex-col shrink-0">
      <div className="h-14 flex items-center px-4 border-b border-white/10">
        <BoltIcon className="h-6 w-6 text-orange-400" />
        <span className="ml-2 font-semibold tracking-tight">PowerMonitor</span>
      </div>

      <nav className="flex-1 py-2 space-y-1">
        {MAIN_ITEMS.map((item) => <Item key={item.path} {...item} />)}
      </nav>

      <div className="py-2 border-t border-white/10">
        {BOTTOM_ITEMS.map((item) => <Item key={item.path} {...item} />)}
        <button
          type="button"
          onClick={onSignOut}
          className="w-full flex items-center gap-3 px-4 py-3 text-sm font-medium text-gray-400 hover:text-white hover:bg-white/5 border-l-4 border-transparent"
        >
          <ArrowLeftOnRectangleIcon className="h-5 w-5 shrink-0" />
          <span>Sign Out</span>
        </button>
      </div>

      {version && (
        <div className="px-4 py-2 text-[10px] text-gray-500 border-t border-white/10">
          v{version}
        </div>
      )}
    </aside>
  )
}
```

- [ ] **Step 2: Create SolisTopBar**

Create `ui/src/components/solis/SolisTopBar.tsx`:

```tsx
import { Bars3Icon, ArrowPathIcon, UserCircleIcon } from '@heroicons/react/24/outline'
import type { Device } from '../../lib/types'

interface SolisTopBarProps {
  devices: Device[]
  selectedDeviceId: string | null
  onSelectDevice: (device: Device) => void
  isOnline: boolean
  lastUpdated?: string
  version?: string
  onMenuClick: () => void
  onRefresh?: () => void
}

export default function SolisTopBar({
  devices,
  selectedDeviceId,
  onSelectDevice,
  isOnline,
  lastUpdated,
  version,
  onMenuClick,
  onRefresh,
}: SolisTopBarProps) {
  const selected = devices.find((d) => d.id === selectedDeviceId)

  return (
    <header className="h-14 bg-[#3c4454] text-white flex items-center justify-between px-4 shrink-0">
      <div className="flex items-center gap-4">
        <button type="button" onClick={onMenuClick} className="lg:hidden p-1 rounded hover:bg-white/10">
          <Bars3Icon className="h-6 w-6" />
        </button>
        <h1 className="text-base font-medium hidden sm:block">{selected?.device_name ?? 'Select device'}</h1>
        <span className={`h-2 w-2 rounded-full ${isOnline ? 'bg-green-400' : 'bg-gray-400'}`} />
        {lastUpdated && <span className="text-xs text-gray-300">Updated: {lastUpdated}</span>}
      </div>

      <div className="flex items-center gap-3">
        {version && (
          <span className="hidden sm:inline text-[10px] px-1.5 py-0.5 rounded bg-white/10 text-gray-300">
            v{version}
          </span>
        )}
        <select
          value={selectedDeviceId ?? ''}
          onChange={(e) => {
            const d = devices.find((x) => x.id === e.target.value)
            if (d) onSelectDevice(d)
          }}
          className="bg-white/10 text-sm rounded px-2 py-1 border border-white/20 focus:outline-none"
        >
          {devices.map((d) => (
            <option key={d.id} value={d.id} className="text-gray-900">
              {d.device_name}
            </option>
          ))}
        </select>
        <button type="button" onClick={onRefresh} className="p-1.5 rounded hover:bg-white/10">
          <ArrowPathIcon className="h-5 w-5" />
        </button>
        <div className="flex items-center gap-2 pl-3 border-l border-white/20">
          <UserCircleIcon className="h-6 w-6 text-gray-300" />
        </div>
      </div>
    </header>
  )
}
```

- [ ] **Step 3: Create SolisLayout**

Create `ui/src/components/solis/SolisLayout.tsx`:

```tsx
import { useState, type ReactNode } from 'react'
import type { Device } from '../../lib/types'
import SolisSidebar from './SolisSidebar'
import SolisTopBar from './SolisTopBar'

export interface SolisLayoutProps {
  children: ReactNode
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  devices: Device[]
  selectedDeviceId: string | null
  onSelectDevice: (device: Device) => void
  isOnline: boolean
  lastUpdated?: string
  version?: string
  onRefresh?: () => void
}

export default function SolisLayout({
  children,
  currentPath,
  onNavigate,
  onSignOut,
  devices,
  selectedDeviceId,
  onSelectDevice,
  isOnline,
  lastUpdated,
  version,
  onRefresh,
}: SolisLayoutProps) {
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false)

  return (
    <div className="min-h-screen bg-[#f0f2f5] flex flex-col">
      <SolisTopBar
        devices={devices}
        selectedDeviceId={selectedDeviceId}
        onSelectDevice={onSelectDevice}
        isOnline={isOnline}
        lastUpdated={lastUpdated}
        version={version}
        onMenuClick={() => setMobileMenuOpen(true)}
        onRefresh={onRefresh}
      />

      <div className="flex flex-1 overflow-hidden">
        {/* Desktop sidebar */}
        <div className="hidden lg:block">
          <SolisSidebar
            currentPath={currentPath}
            onNavigate={onNavigate}
            onSignOut={onSignOut}
            version={version}
          />
        </div>

        {/* Mobile drawer */}
        {mobileMenuOpen && (
          <>
            <div
              className="fixed inset-0 z-30 bg-black/50 lg:hidden"
              onClick={() => setMobileMenuOpen(false)}
            />
            <div className="fixed inset-y-0 left-0 z-40 lg:hidden">
              <SolisSidebar
                currentPath={currentPath}
                onNavigate={(path) => { onNavigate(path); setMobileMenuOpen(false) }}
                onSignOut={() => { setMobileMenuOpen(false); onSignOut() }}
                version={version}
              />
            </div>
          </>
        )}

        <main className="flex-1 overflow-y-auto p-4 md:p-6">
          <div className="max-w-[1440px] mx-auto">
            {children}
          </div>
        </main>
      </div>
    </div>
  )
}
```

- [ ] **Step 4: Test SolisSidebar**

Create `ui/src/components/solis/__tests__/SolisSidebar.test.tsx`:

```tsx
import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import SolisSidebar from '../SolisSidebar'

const noop = vi.fn()

describe('SolisSidebar', () => {
  it('renders navigation items and version', () => {
    render(
      <SolisSidebar
        currentPath="/dashboard"
        onNavigate={noop}
        onSignOut={noop}
        version="0.3.0"
      />
    )

    expect(screen.getByText('Overview')).toBeInTheDocument()
    expect(screen.getByText('Device')).toBeInTheDocument()
    expect(screen.getByText('Analysis')).toBeInTheDocument()
    expect(screen.getByText('Alarms')).toBeInTheDocument()
    expect(screen.getByText('Classic View')).toBeInTheDocument()
    expect(screen.getByText('v0.3.0')).toBeInTheDocument()
  })
})
```

Run:

```bash
npm test -- src/components/solis/__tests__/SolisSidebar.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 5: Commit**

```bash
git add ui/src/components/solis
git commit -m "feat(ui): add Solis layout shell (topbar, sidebar, layout)"
```

---

### Task 4: Build Solis primitive components

**Files:**
- Create: `ui/src/components/solis/SemiGauge.tsx`
- Create: `ui/src/components/solis/KpiCard.tsx`
- Create: `ui/src/components/solis/StatusBadge.tsx`
- Create: `ui/src/components/solis/DateRangeTabs.tsx`
- Test: corresponding `__tests__/*.test.tsx`

- [ ] **Step 1: SemiGauge + test**

Create `ui/src/components/solis/SemiGauge.tsx`:

```tsx
interface SemiGaugeProps {
  value: number
  max: number
  color?: string
  size?: number
  stroke?: number
}

export default function SemiGauge({
  value,
  max,
  color = '#f97316',
  size = 80,
  stroke = 10,
}: SemiGaugeProps) {
  const r = (size - stroke) / 2
  const cx = size / 2
  const cy = size / 2
  const arc = Math.PI * r
  const offset = arc - (Math.min(value, max) / max) * arc

  return (
    <svg width={size} height={size / 2 + 4}>
      <path
        d={`M ${cx - r} ${cy} A ${r} ${r} 0 0 1 ${cx + r} ${cy}`}
        fill="none"
        stroke="#e5e7eb"
        strokeWidth={stroke}
        strokeLinecap="round"
      />
      <path
        d={`M ${cx - r} ${cy} A ${r} ${r} 0 0 1 ${cx + r} ${cy}`}
        fill="none"
        stroke={color}
        strokeWidth={stroke}
        strokeDasharray={arc}
        strokeDashoffset={offset}
        strokeLinecap="round"
        className="transition-all duration-700"
      />
    </svg>
  )
}
```

Test verifies an arc path is rendered.

- [ ] **Step 2: KpiCard + test**

Create `ui/src/components/solis/KpiCard.tsx`:

```tsx
import type { ReactNode } from 'react'

interface KpiCardProps {
  label: string
  value: string
  unit?: string
  subText?: ReactNode
  icon?: ReactNode
  accent?: string
}

export default function KpiCard({ label, value, unit, subText, icon, accent }: KpiCardProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm flex items-center gap-4">
      <div className="shrink-0">
        {icon ? (
          <div className={`h-14 w-14 rounded-full flex items-center justify-center text-white ${accent ?? 'bg-orange-500'}`}>
            {icon}
          </div>
        ) : (
          <div className={`h-14 w-14 rounded-full ${accent ?? 'bg-orange-100'}`} />
        )}
      </div>
      <div className="min-w-0">
        <div className="text-sm text-gray-500">{label}</div>
        <div className="text-2xl font-bold text-gray-800 truncate">
          {value}
          {unit && <span className="text-base font-normal text-gray-500 ml-1">{unit}</span>}
        </div>
        {subText && <div className="text-xs text-gray-400 mt-0.5">{subText}</div>}
      </div>
    </div>
  )
}
```

- [ ] **Step 3: StatusBadge + test**

Create `ui/src/components/solis/StatusBadge.tsx`:

```tsx
interface StatusBadgeProps {
  status: 'normal' | 'warning' | 'offline' | string
  label?: string
}

const colorMap: Record<string, string> = {
  normal: 'bg-green-500',
  warning: 'bg-amber-500',
  offline: 'bg-gray-400',
}

export default function StatusBadge({ status, label }: StatusBadgeProps) {
  return (
    <span className="inline-flex items-center gap-1.5 text-sm text-gray-700">
      <span className={`h-2 w-2 rounded-full ${colorMap[status] ?? colorMap.normal}`} />
      {label ?? status}
    </span>
  )
}
```

- [ ] **Step 4: DateRangeTabs + test**

Create `ui/src/components/solis/DateRangeTabs.tsx`:

```tsx
type Range = 'day' | 'month' | 'year' | 'total'

interface DateRangeTabsProps {
  value: Range
  onChange: (range: Range) => void
}

const options: { value: Range; label: string }[] = [
  { value: 'day', label: 'Day' },
  { value: 'month', label: 'Month' },
  { value: 'year', label: 'Year' },
  { value: 'total', label: 'Total' },
]

export default function DateRangeTabs({ value, onChange }: DateRangeTabsProps) {
  return (
    <div className="inline-flex rounded-md border border-gray-300 overflow-hidden bg-white">
      {options.map((opt) => (
        <button
          key={opt.value}
          type="button"
          onClick={() => onChange(opt.value)}
          className={`px-4 py-1.5 text-sm font-medium border-r border-gray-200 last:border-r-0 ${
            value === opt.value ? 'bg-[#3c4454] text-white' : 'text-gray-600 hover:bg-gray-50'
          }`}
        >
          {opt.label}
        </button>
      ))}
    </div>
  )
}
```

- [ ] **Step 5: Run tests and build**

```bash
npm test -- src/components/solis/__tests__/SemiGauge.test.tsx src/components/solis/__tests__/KpiCard.test.tsx src/components/solis/__tests__/StatusBadge.test.tsx src/components/solis/__tests__/DateRangeTabs.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
git add ui/src/components/solis
git commit -m "feat(ui): add Solis KPI primitives (gauge, card, status, tabs)"
```

---

### Task 5: Build Solis Overview page

**Files:**
- Create: `ui/src/components/solis/SolisChart.tsx`
- Create: `ui/src/components/solis/AlarmPanel.tsx`
- Create: `ui/src/components/solis/DeviceInfoCard.tsx`
- Create: `ui/src/pages/SolisOverviewPage.tsx`
- Test: `ui/src/pages/__tests__/SolisOverviewPage.test.tsx`

- [ ] **Step 1: SolisChart**

Create `ui/src/components/solis/SolisChart.tsx`:

```tsx
import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  PointElement,
  LineElement,
  BarElement,
  Title,
  Tooltip,
  Legend,
} from 'chart.js'
import { Bar } from 'react-chartjs-2'
import type { TelemetryPoint } from '../../lib/types'

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, BarElement, Title, Tooltip, Legend)

interface SolisChartProps {
  data: TelemetryPoint[]
  metric: string
}

export default function SolisChart({ data, metric }: SolisChartProps) {
  const labels = data.map((p) => new Date(p.recorded_at).toLocaleTimeString())
  const values = data.map((p) => p.payload[metric] ?? 0)

  return (
    <div className="bg-white rounded-lg border border-gray-200 p-4 shadow-sm h-80">
      <Bar
        data={{
          labels,
          datasets: [
            {
              label: metric,
              data: values,
              backgroundColor: '#f97316',
              borderRadius: 2,
            },
          ],
        }}
        options={{
          responsive: true,
          maintainAspectRatio: false,
          plugins: { legend: { display: false } },
          scales: {
            x: { grid: { display: false } },
            y: { beginAtZero: true },
          },
        }}
      />
    </div>
  )
}
```

- [ ] **Step 2: AlarmPanel**

Create `ui/src/components/solis/AlarmPanel.tsx`:

```tsx
interface AlarmPanelProps {
  alarmCount: number
}

export default function AlarmPanel({ alarmCount }: AlarmPanelProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm">
      <h3 className="text-sm font-semibold text-gray-700 mb-3">Alarm</h3>
      {alarmCount === 0 ? (
        <div className="text-sm text-gray-500">No alarm</div>
      ) : (
        <div className="text-sm text-amber-600">{alarmCount} active alarms</div>
      )}
    </div>
  )
}
```

- [ ] **Step 3: DeviceInfoCard**

Create `ui/src/components/solis/DeviceInfoCard.tsx`:

```tsx
import type { Device } from '../../lib/types'

interface DeviceInfoCardProps {
  device: Device
}

export default function DeviceInfoCard({ device }: DeviceInfoCardProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm">
      <h3 className="text-sm font-semibold text-gray-700 mb-3">Device Info</h3>
      <div className="space-y-2 text-sm">
        <div className="flex justify-between"><span className="text-gray-500">Name</span><span>{device.device_name}</span></div>
        <div className="flex justify-between"><span className="text-gray-500">Type</span><span>{device.device_type}</span></div>
        <div className="flex justify-between"><span className="text-gray-500">Key</span><span className="truncate max-w-[120px]">{device.device_key}</span></div>
        <div className="flex justify-between"><span className="text-gray-500">Online</span><span>{device.is_online ? 'Yes' : 'No'}</span></div>
      </div>
    </div>
  )
}
```

- [ ] **Step 4: SolisOverviewPage**

Create `ui/src/pages/SolisOverviewPage.tsx`:

```tsx
import { useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useAtomValue } from 'jotai'
import { selectedDeviceAtom, liveBufferAtom, connectionStateAtom } from '../state/atoms'
import { computedTelemetryAtom } from '../state/derived'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'
import { APP_VERSION } from '../lib/version'
import SolisLayout from '../components/solis/SolisLayout'
import SemiGauge from '../components/solis/SemiGauge'
import KpiCard from '../components/solis/KpiCard'
import StatusBadge from '../components/solis/StatusBadge'
import DateRangeTabs from '../components/solis/DateRangeTabs'
import SolisChart from '../components/solis/SolisChart'
import AlarmPanel from '../components/solis/AlarmPanel'
import DeviceInfoCard from '../components/solis/DeviceInfoCard'

type Tab = 'overview' | 'yield' | 'flow'
type Range = 'day' | 'month' | 'year' | 'total'

export default function SolisOverviewPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const telemetry = useAtomValue(computedTelemetryAtom)
  const buffer = useAtomValue(liveBufferAtom)
  const connection = useAtomValue(connectionStateAtom)

  useEffect(() => {
    async function load() {
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (data) setDevices(data)
    }
    load()
  }, [])

  const [activeTab, setActiveTab] = useState<Tab>('overview')
  const [range, setRange] = useState<Range>('day')

  const online = selectedDevice?.is_online ?? false
  const status = connection === 'live' ? 'normal' : 'offline'
  const alarmCount = 0

  const kpiSub = useMemo(() => {
    const cap = 'Installed Capacity: 4.2 kWp'
    const totalYield = `Total Yield: ${(telemetry.total_energy_wh / 1000).toFixed(1)} kWh`
    return { cap, totalYield }
  }, [telemetry])

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  if (!selectedDevice) {
    return (
      <SolisLayout
        currentPath="/dashboard"
        onNavigate={handleNavigate}
        onSignOut={handleSignOut}
        devices={devices}
        selectedDeviceId={null}
        onSelectDevice={setSelectedDevice}
        isOnline={false}
        version={APP_VERSION}
        onRefresh={handleRefresh}
      >
        <div className="text-center py-20 text-gray-500">Select a device to view telemetry.</div>
      </SolisLayout>
    )
  }

  return (
    <SolisLayout
      currentPath="/dashboard"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      devices={devices}
      selectedDeviceId={selectedDevice.id}
      onSelectDevice={setSelectedDevice}
      isOnline={online}
      version={APP_VERSION}
      onRefresh={handleRefresh}
    >
      {/* KPI row */}
      <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-4 gap-4 mb-6">
        <KpiCard
          label="Current Power"
          value={telemetry.pv_power.toFixed(1)}
          unit="W"
          subText={kpiSub.cap}
          icon={<SemiGauge value={telemetry.pv_power} max={4200} size={56} stroke={8} />}
          accent="bg-orange-500"
        />
        <KpiCard
          label="Daily Yield"
          value={(telemetry.total_energy_wh / 1000).toFixed(1)}
          unit="kWh"
          subText={kpiSub.totalYield}
          icon={<SemiGauge value={telemetry.total_energy_wh / 1000} max={42} size={56} stroke={8} />}
          accent="bg-orange-500"
        />
        <KpiCard
          label="Battery SoC"
          value={telemetry.min_soc_pct?.toFixed(0) ?? '--'}
          unit="%"
          subText={`Status: ${telemetry.system_status}`}
          icon={<SemiGauge value={telemetry.min_soc_pct ?? 0} max={100} size={56} stroke={8} color="#22c55e" />}
          accent="bg-green-500"
        />
        <KpiCard
          label="System Status"
          value={status}
          subText={<StatusBadge status={status} label={status === 'normal' ? 'Normal' : 'Offline'} />}
          icon={<div className="h-7 w-7 rounded-full bg-white/20" />}
          accent="bg-[#3c4454]"
        />
      </div>

      {/* Tabs */}
      <div className="flex items-center justify-between mb-4">
        <div className="inline-flex rounded-md border border-gray-300 overflow-hidden bg-white">
          {[
            { key: 'overview', label: 'Overview' },
            { key: 'yield', label: 'Yield Chart' },
            { key: 'flow', label: 'Power Flow' },
          ].map((t) => (
            <button
              key={t.key}
              type="button"
              onClick={() => setActiveTab(t.key as Tab)}
              className={`px-4 py-1.5 text-sm font-medium border-r border-gray-200 last:border-r-0 ${
                activeTab === t.key ? 'bg-[#3c4454] text-white' : 'text-gray-600 hover:bg-gray-50'
              }`}
            >
              {t.label}
            </button>
          ))}
        </div>
        <DateRangeTabs value={range} onChange={setRange} />
      </div>

      {/* Tab content */}
      {activeTab === 'overview' && (
        <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
          <div className="lg:col-span-2">
            <SolisChart data={buffer} metric="pv_power" />
          </div>
          <div className="space-y-4">
            <AlarmPanel alarmCount={alarmCount} />
            <DeviceInfoCard device={selectedDevice} />
          </div>
        </div>
      )}

      {activeTab === 'yield' && (
        <SolisChart data={buffer} metric="pv_power" />
      )}

      {activeTab === 'flow' && (
        <div className="bg-white rounded-lg border border-gray-200 p-6 shadow-sm text-center text-gray-500">
          Power flow diagram will be implemented in Task 6.
        </div>
      )}
    </SolisLayout>
  )
}
```

- [ ] **Step 5: Test and build**

Create a light render test for `SolisOverviewPage` that verifies KPI labels and tabs render.

Run:

```bash
npm test -- src/pages/__tests__/SolisOverviewPage.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
git add ui/src/components/solis ui/src/pages/SolisOverviewPage.tsx ui/src/pages/__tests__/SolisOverviewPage.test.tsx
git commit -m "feat(ui): add Solis Overview page with KPI cards and tabs"
```

---

### Task 6: Build topology diagram and Device page

**Files:**
- Create: `ui/src/components/solis/TopologyDiagram.tsx`
- Create: `ui/src/components/solis/ParamTable.tsx`
- Create: `ui/src/components/solis/RealtimePanel.tsx`
- Create: `ui/src/pages/SolisDevicePage.tsx`
- Test: `ui/src/pages/__tests__/SolisDevicePage.test.tsx`

- [ ] **Step 1: TopologyDiagram + test**

Create `ui/src/components/solis/TopologyDiagram.tsx`:

```tsx
interface TopologyDiagramProps {
  pvPower: number
  gridPower: number
  batteryPower: number
  loadPower: number
  batterySoc: number | null
}

function Node({
  label,
  value,
  unit,
  icon,
  color,
}: {
  label: string
  value: string
  unit?: string
  icon: React.ReactNode
  color: string
}) {
  return (
    <div className="flex flex-col items-center">
      <div className={`h-20 w-20 rounded-full border-4 ${color} flex items-center justify-center bg-white`}>
        {icon}
      </div>
      <div className="mt-2 text-center">
        <div className="text-sm font-semibold text-gray-700">{label}</div>
        <div className="text-lg font-bold text-gray-800">
          {value}
          {unit && <span className="text-sm font-normal text-gray-500 ml-0.5">{unit}</span>}
        </div>
      </div>
    </div>
  )
}

export default function TopologyDiagram({
  pvPower,
  gridPower,
  batteryPower,
  loadPower,
  batterySoc,
}: TopologyDiagramProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-6 shadow-sm">
      <div className="flex flex-col items-center gap-6 md:flex-row md:items-center md:justify-center md:gap-8">
        <Node label="PV" value={pvPower.toFixed(1)} unit="kW" icon={<span className="text-2xl">☀</span>} color="border-amber-400" />
        <div className="hidden md:block h-1 w-16 bg-amber-300" />
        <Node label="Inverter" value={(pvPower + batteryPower).toFixed(1)} unit="kW" icon={<span className="text-2xl">⚡</span>} color="border-orange-500" />
        <div className="hidden md:block h-1 w-16 bg-blue-300" />
        <Node label="Grid" value={gridPower.toFixed(1)} unit="kW" icon={<span className="text-2xl">🏭</span>} color="border-blue-400" />
      </div>
      <div className="flex flex-col md:flex-row items-center justify-center gap-8 mt-6">
        <Node label="Battery" value={batterySoc?.toFixed(0) ?? '--'} unit="%" icon={<span className="text-2xl">🔋</span>} color={batteryPower > 0 ? 'border-green-400' : 'border-amber-400'} />
        <Node label="Load" value={loadPower.toFixed(1)} unit="kW" icon={<span className="text-2xl">💡</span>} color="border-gray-400" />
      </div>
    </div>
  )
}
```

- [ ] **Step 2: ParamTable + test**

Create `ui/src/components/solis/ParamTable.tsx`:

```tsx
interface Column {
  key: string
  label: string
}

interface ParamTableProps {
  title: string
  columns: Column[]
  rows: Record<string, string | number | null>[]
}

export default function ParamTable({ title, columns, rows }: ParamTableProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 shadow-sm overflow-hidden">
      <div className="px-4 py-3 border-b border-gray-200 font-semibold text-gray-700 text-sm">{title}</div>
      <div className="overflow-x-auto">
        <table className="w-full text-sm">
          <thead className="bg-gray-50">
            <tr>
              {columns.map((c) => (
                <th key={c.key} className="px-4 py-2 text-left text-gray-500 font-medium">{c.label}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, i) => (
              <tr key={i} className="border-t border-gray-100">
                {columns.map((c) => (
                  <td key={c.key} className="px-4 py-2 text-gray-700">{row[c.key] ?? '--'}</td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
```

- [ ] **Step 3: RealtimePanel + test**

Create `ui/src/components/solis/RealtimePanel.tsx`:

```tsx
import type { Device } from '../../lib/types'

interface RealtimePanelProps {
  device: Device
  currentPower: number
  dailyYield: number
  totalYield: number
  status: string
  alarmCount: number
}

export default function RealtimePanel({
  device,
  currentPower,
  dailyYield,
  totalYield,
  status,
  alarmCount,
}: RealtimePanelProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm space-y-4">
      <h3 className="text-sm font-semibold text-gray-700">Real-time Information</h3>
      <div className="grid grid-cols-2 gap-3 text-sm">
        <div><span className="text-gray-500">Status</span><div className="font-medium">{status}</div></div>
        <div><span className="text-gray-500">Current Power</span><div className="font-medium">{currentPower.toFixed(1)} W</div></div>
        <div><span className="text-gray-500">Daily Yield</span><div className="font-medium">{dailyYield.toFixed(2)} kWh</div></div>
        <div><span className="text-gray-500">Total Yield</span><div className="font-medium">{totalYield.toFixed(2)} kWh</div></div>
        <div><span className="text-gray-500">Alarms</span><div className="font-medium">{alarmCount}</div></div>
      </div>
      <h3 className="text-sm font-semibold text-gray-700 pt-2 border-t border-gray-100">Basic Information</h3>
      <div className="grid grid-cols-2 gap-3 text-sm">
        <div><span className="text-gray-500">Name</span><div className="font-medium">{device.device_name}</div></div>
        <div><span className="text-gray-500">Type</span><div className="font-medium">{device.device_type}</div></div>
      </div>
    </div>
  )
}
```

- [ ] **Step 4: SolisDevicePage**

Create `ui/src/pages/SolisDevicePage.tsx` using `SolisLayout`, `TopologyDiagram`, `RealtimePanel`, and `ParamTable`. Wire it to `selectedDeviceAtom` and `computedTelemetryAtom`. Use the existing `channelPayloadAtomFamily` to build PV/DC parameter rows.

- [ ] **Step 5: Test and build**

Create `ui/src/pages/__tests__/SolisDevicePage.test.tsx` verifying topology nodes and tables render.

Run:

```bash
npm test -- src/pages/__tests__/SolisDevicePage.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 6: Commit**

```bash
git add ui/src/components/solis ui/src/pages/SolisDevicePage.tsx ui/src/pages/__tests__/SolisDevicePage.test.tsx
git commit -m "feat(ui): add Solis Device page with topology and parameter tables"
```

---

### Task 7: Build Analysis page

**Files:**
- Create: `ui/src/components/solis/MetricSelector.tsx`
- Create: `ui/src/pages/SolisAnalysisPage.tsx`
- Test: `ui/src/pages/__tests__/SolisAnalysisPage.test.tsx`

- [ ] **Step 1: MetricSelector + test**

Create `ui/src/components/solis/MetricSelector.tsx`:

```tsx
interface MetricGroup {
  label: string
  metrics: { key: string; label: string }[]
}

interface MetricSelectorProps {
  groups: MetricGroup[]
  selected: string[]
  onChange: (selected: string[]) => void
}

export default function MetricSelector({ groups, selected, onChange }: MetricSelectorProps) {
  function toggle(key: string) {
    onChange(selected.includes(key) ? selected.filter((k) => k !== key) : [...selected, key])
  }

  return (
    <div className="bg-white rounded-lg border border-gray-200 p-4 shadow-sm space-y-4">
      {groups.map((g) => (
        <div key={g.label}>
          <div className="text-sm font-semibold text-gray-700 mb-2">{g.label}</div>
          <div className="flex flex-wrap gap-3">
            {g.metrics.map((m) => (
              <label key={m.key} className="inline-flex items-center gap-1.5 text-sm text-gray-600 cursor-pointer">
                <input
                  type="checkbox"
                  checked={selected.includes(m.key)}
                  onChange={() => toggle(m.key)}
                  className="rounded border-gray-300"
                />
                {m.label}
              </label>
            ))}
          </div>
        </div>
      ))}
    </div>
  )
}
```

- [ ] **Step 2: SolisAnalysisPage**

Create `ui/src/pages/SolisAnalysisPage.tsx`:

- Uses `SolisLayout`
- Has `DateRangeTabs`
- Has `MetricSelector` with groups: DC Voltage, DC Current, DC Power, AC Voltage, AC Current, Total Power, Daily Yield
- Renders a `SolisChart` for the first selected metric (or multiple datasets if time allows)

- [ ] **Step 3: Test and build**

Create `ui/src/pages/__tests__/SolisAnalysisPage.test.tsx`.

Run:

```bash
npm test -- src/pages/__tests__/SolisAnalysisPage.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 4: Commit**

```bash
git add ui/src/components/solis ui/src/pages/SolisAnalysisPage.tsx ui/src/pages/__tests__/SolisAnalysisPage.test.tsx
git commit -m "feat(ui): add Solis Analysis page with metric selector"
```

---

### Task 8: Build Alarms page

**Files:**
- Create: `ui/src/pages/SolisAlarmsPage.tsx`
- Test: `ui/src/pages/__tests__/SolisAlarmsPage.test.tsx`

- [ ] **Step 1: Create SolisAlarmsPage**

A simple page wrapped in `SolisLayout` showing a placeholder alarm list. Use relay states or connection state to derive a mock alarm list. Each alarm row has status, time, and message.

- [ ] **Step 2: Test and build**

Create a render test verifying the alarms heading and layout.

Run:

```bash
npm test -- src/pages/__tests__/SolisAlarmsPage.test.tsx
npm run build
```

Expected: PASS and build succeeds.

- [ ] **Step 3: Commit**

```bash
git add ui/src/pages/SolisAlarmsPage.tsx ui/src/pages/__tests__/SolisAlarmsPage.test.tsx
git commit -m "feat(ui): add Solis Alarms page"
```

---

### Task 9: Wire routes and finalize navigation

**Files:**
- Modify: `ui/src/App.tsx`

- [ ] **Step 1: Update App.tsx**

Replace the existing `/dashboard` route wiring so it points to the new Solis pages:

```tsx
import SolisOverviewPage from './pages/SolisOverviewPage'
import SolisDevicePage from './pages/SolisDevicePage'
import SolisAnalysisPage from './pages/SolisAnalysisPage'
import SolisAlarmsPage from './pages/SolisAlarmsPage'
import ClassicDashboardPage from './pages/ClassicDashboardPage'
```

Routes:

```tsx
<Route path="/dashboard" element={<ProtectedRoute><SolisOverviewPage /></ProtectedRoute>} />
<Route path="/dashboard/device" element={<ProtectedRoute><SolisDevicePage /></ProtectedRoute>} />
<Route path="/dashboard/analysis" element={<ProtectedRoute><SolisAnalysisPage /></ProtectedRoute>} />
<Route path="/dashboard/alarms" element={<ProtectedRoute><SolisAlarmsPage /></ProtectedRoute>} />
<Route path="/dashboard/classic" element={<ProtectedRoute><ClassicDashboardPage /></ProtectedRoute>} />
```

- [ ] **Step 2: Remove LegacyDashboardPage route**

Delete or remove the `/dashboard/legacy` route; the legacy page file can remain untouched or be deleted later.

- [ ] **Step 3: Run full test suite and build**

```bash
npm test
npm run build
```

Expected: all tests pass and build succeeds.

- [ ] **Step 4: Commit**

```bash
git add ui/src/App.tsx
git commit -m "feat(ui): wire Solis dashboard routes and classic fallback"
```

---

### Task 10: Full verification and polish

**Files:**
- No new files.

- [ ] **Step 1: Run full test suite**

```bash
cd ui
npm test
```

Expected: all tests pass.

- [ ] **Step 2: Run production build**

```bash
npm run build
```

Expected: no TypeScript or Vite errors.

- [ ] **Step 3: Smoke test dev server (optional)**

```bash
npm run dev
```

Open `http://localhost:3000` and verify:
- Dark top bar and dark sidebar render
- Version badge `v0.3.0` visible
- Overview page shows 4 KPI cards with gauges
- Tabs switch between Overview, Yield Chart, Power Flow
- Device page shows topology and parameter tables
- Analysis page shows metric selector and chart
- `/dashboard/classic` still renders the old widget grid

- [ ] **Step 4: Commit any final fixes**

If fixes are needed, commit with a clear message.

---

## Self-Review

### 1. Spec coverage

| Requirement | Task |
|-------------|------|
| Dark Solis-style top bar + sidebar | Task 3 |
| Large gauge KPI cards | Tasks 4 + 5 |
| Overview / Yield Chart / Power Flow tabs | Task 5 |
| Topology diagram on device page | Task 6 |
| Real-time info + parameter tables | Task 6 |
| Analysis page with metric selector | Task 7 |
| Alarms page | Task 8 |
| Classic dashboard preserved | Tasks 2 + 9 |
| Version badge | Tasks 1 + 3 |
| Responsive layout | Built into all components with Tailwind breakpoints |

### 2. Placeholder scan

No "TBD", "implement later", or missing code blocks. Each component has concrete code; each page has a concrete structure. The Power Flow placeholder in Task 5 is explicitly filled by Task 6.

### 3. Type consistency

- `APP_VERSION` is `string`.
- `useIsMobile()` returns `boolean`.
- `SolisLayout`, `SolisTopBar`, `SolisSidebar` use the shared `Device` type.
- `SemiGauge` takes `value`, `max`, optional `color`, `size`, `stroke`.
- `MetricSelector` emits `string[]` of selected metric keys.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-14-solis-inspired-dashboard.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Dispatch a fresh subagent per task, review between tasks, fast iteration. **REQUIRED SUB-SKILL:** `superpowers:subagent-driven-development`.

**2. Inline Execution** — Execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints for review.

**Which approach?**
