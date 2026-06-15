# Solis-Inspired Dashboard Design Spec

## Goal

Redesign the web dashboard to match the visual language and information architecture of the Solis inverter monitoring portal: clean, industrial, power-station feel with a dark top bar, dark icon sidebar, large gauge KPI cards, topology/flow diagrams, tabbed tables, and clear alarm/status panels.

Keep the existing widget-based dashboard as **Classic** and show the app version in the UI.

---

## Visual Language

### Color Palette

| Token | Value | Usage |
|-------|-------|-------|
| `--topbar-bg` | `#3c4454` | Top header bar |
| `--sidebar-bg` | `#2e3440` | Left navigation sidebar |
| `--page-bg` | `#f0f2f5` | Main page background |
| `--card-bg` | `#ffffff` | Card surfaces |
| `--accent-orange` | `#f97316` | Power, yield, primary actions |
| `--accent-green` | `#22c55e` | Online / charging / normal |
| `--accent-amber` | `#f59e0b` | Warning / offline / discharging |
| `--accent-blue` | `#3b82f6` | Grid / info / links |
| `--accent-slate` | `#64748b` | Inactive / disabled |
| `--text-primary` | `#1f2937` | Headings, primary values |
| `--text-secondary` | `#6b7280` | Labels, captions |
| `--border` | `#e5e7eb` | Card borders, dividers |

### Typography

- Primary values: `text-2xl font-bold text-gray-800`
- Card labels: `text-sm text-gray-500`
- Sub-labels: `text-xs text-gray-400`
- Section headings: `text-lg font-semibold text-gray-800`
- Sidebar labels: `text-sm font-medium`

### Layout Grid

- Desktop: fixed dark left sidebar (64 px collapsed icons, 200 px expanded labels), fluid main area
- Main area max-width: `1440px` centered
- Card grid: 4-column on large, 2-column on medium, 1-column on mobile
- Card padding: `24px`, border-radius: `8px`, subtle shadow

---

## Navigation Structure

### Left Sidebar (dark, icon-first)

```
┌─────────────────────┐
│  ⚡ Logo / Brand    │
├─────────────────────┤
│ 📊  Overview        │
│ 🔌  Plant / Device  │
│ ⚡  Channels        │
│ 📈  Analysis        │
│ 🔔  Alarms          │
├─────────────────────┤
│ ⚙️  Settings        │
│ 🖥️  Classic View   │
│ 🚪  Sign Out        │
└─────────────────────┘
```

- Collapsed by default on desktop; expands on hover or click
- Mobile: drawer overlay

### Top Bar (dark navy)

Left:
- Hamburger / sidebar toggle
- Device selector dropdown (current device name + online dot)
- "Last updated: 2s ago"

Right:
- Refresh button
- User avatar menu (Basic Settings, My Info, Sign Out)
- Version badge `v0.2.0` near the user menu

---

## Page 1: Overview (Dashboard)

### KPI Summary Row

Four white cards, each with a **semi-circular gauge** on the left and value stack on the right:

1. **Current Power**
   - Gauge: orange arc from 0° to ~180°, filled by current power / max capacity
   - Value: `0 W` (large)
   - Sub: `Installed Capacity: 42.6 kWp`

2. **Daily Yield**
   - Gauge: orange lightning icon + mini gauge
   - Value: `0 kWh`
   - Sub: `Monthly Yield: 51 kWh` / `Total Yield: 6.89 MWh`

3. **Battery State**
   - Gauge: battery arc with SoC
   - Value: `86 %`
   - Sub: `Status: Charging` / `Today's Charge: 2.1 kWh`

4. **System Status**
   - Icon panel (no gauge): online dot + alarm count + relay count
   - Value: `Normal`
   - Sub: `Alarms: 0` / `Relays: 2 ON`

### Main Content Area

Below the KPI row:

- **Tabs**: `Overview` | `Yield Chart` | `Power Flow`
- **Overview tab** (default):
  - Left 2/3: `Daily Yield` time-series chart (bar/line), with date picker and Day/Month/Year/Total range
  - Right 1/3: stacked info cards:
    - Alarm status: "No alarm" or list of active alarms
    - Device info: plant name, installed capacity, location, timezone
    - Today's weather / environmental (optional placeholder)
- **Yield Chart tab**: full-width focused chart with metric selector checkboxes (DC Voltage, DC Current, DC Power, AC Voltage, AC Current, Total Power, Daily Yield, etc.)
- **Power Flow tab**: topology diagram for the selected device

---

## Page 2: Device / Inverter Detail

### Header

- Title: `Inverter Details` / `Device Overview`
- Subtitle: SN / Plant ID / Last updated
- Action buttons: Refresh, Inverter Control, Delete

### Topology Diagram (centerpiece)

A visual energy-flow diagram with rounded nodes connected by colored lines:

```
        [PV]
         │
         ▼
    [Inverter] ◄────── [Grid]
         │
         ▼
    [Battery]        [Load]
```

- Nodes are circles with icons and power values
- Lines animate or change color based on direction
- Values update live from telemetry
- Clicking a node reveals details in a side panel

### Real-Time Info Panel (right side)

Two sections:

1. **Real-time Information**
   - Status: Online (green dot)
   - Current Power
   - Daily / Monthly / Annual Yield
   - Total Yield
   - Alarm count
   - Temperature (if available)

2. **Basic Information**
   - Name / Serial
   - Rated Power
   - Inverter Version
   - Plant name link
   - Model

### Parameter Tables

Below the topology:

- **PV Section**: Power (kW), Daily Yield (kWh), Total Yield (kWh) per string
- **Grid Section**: Power (Charge/Discharge), Energy From Grid, Energy To Grid
- **Battery Section**: Power, Status (Charge/Discharge), SoC
- **Load Section**: Power, Daily Consumption, Total Consumption
- **DC Parameters**: U (V), I (A), P (W) per PV input
- **AC Parameters**: U (V), I (A), F (Hz) per phase

---

## Page 3: Device Overview / Plant List

When user has multiple devices:

- **Summary cards**: Inverter (Total/Normal/Alarm/Offline), Datalogger, EPM, Battery
- **Tabbed table**: Plant List / Plant Location / Plant Chart
- Table columns: Status, Device Name, Current Power, Daily Yield, Total Yield, Last Update, Operation
- Status filters: All / Online / Offline / Alarm

For this project scope, map "Plant" to "Device" and "Inverter" to the selected device instance.

---

## Page 4: Analysis

- Date range picker + Day / Month / Year / Total tabs
- Analysis type tabs: Recommended / DC Analysis / AC Analysis / Output Analysis
- Metric checkboxes grouped by:
  - DC: Voltage (pv1..pv4), Current, Power
  - AC: Voltage, Current, Frequency
  - Output: Total Power, Daily Yield, Total Yield, Power Factor
- Chart renders selected metrics
- Export button (placeholder)

---

## Responsive Behavior

### Desktop (≥1024 px)

- Full dark sidebar (expanded or collapsed)
- 4-column KPI row
- 2/3 + 1/3 main layout
- Topology + side panel side-by-side

### Tablet (768–1023 px)

- Sidebar collapses to icons-only or hidden
- 2-column KPI row
- Main content single column
- Topology stacks above info panel

### Mobile (<768 px)

- Top bar with hamburger + device selector + user menu
- Sidebar becomes drawer
- KPI cards stack vertically
- Each card remains full-width with large tap targets
- Topology diagram simplified to vertical flow
- Tables become horizontal scroll or card lists

---

## Components Required

| Component | Purpose |
|-----------|---------|
| `SolisLayout` | Dark top bar + dark sidebar wrapper |
| `SolisSidebar` | Icon-first dark navigation with groups |
| `SolisTopBar` | Device selector, refresh, user menu, version badge |
| `SemiGauge` | Semi-circular SVG gauge for KPI cards |
| `KpiCard` | Large white summary card with gauge or icon |
| `StatusBadge` | Green/orange/gray dot + text status |
| `TopologyDiagram` | Energy flow nodes and animated lines |
| `RealtimePanel` | Inverter/device real-time details |
| `ParamTable` | DC/AC parameter table |
| `MetricSelector` | Checkbox groups for analysis chart |
| `DateRangeTabs` | Day/Month/Year/Total segmented control |
| `SolisChart` | Reuse existing chart with Solis styling |
| `AlarmPanel` | Alarm status / list panel |
| `DeviceInfoCard` | Plant/device info card |

---

## Data Mapping

Use existing Jotai atoms:

- `selectedDeviceAtom` → current plant/device
- `latestAtom` / `computedTelemetryAtom` → live power/yield/status
- `relayStatesAtomFamily` → relay counts
- `liveBufferAtom` → time-series data for charts
- `deviceChannelsAtomFamily` → channel names / PV strings

KPI mapping:

| KPI | Source |
|-----|--------|
| Current Power | `computedTelemetryAtom.pv_power` or total system power |
| Daily Yield | `computedTelemetryAtom.total_energy_wh / 1000` |
| Battery SoC | `computedTelemetryAtom.min_soc_pct` |
| System Status | `computedTelemetryAtom.system_status` |
| Alarms | derive from relay flags / connection state |

---

## Acceptance Criteria

- [ ] Dark top bar and dark sidebar render on all routes
- [ ] Version badge `v0.2.0` visible in top bar or sidebar
- [ ] Overview page shows 4 KPI cards with semi-circular gauges
- [ ] Overview page has Overview / Yield Chart / Power Flow tabs
- [ ] Yield chart uses existing telemetry data with Day/Month/Year/Total range
- [ ] Power Flow tab renders topology diagram with live values
- [ ] Device Detail page shows topology + real-time panel + parameter tables
- [ ] Analysis page has metric selectors and date range tabs
- [ ] Responsive layout works on mobile, tablet, and desktop
- [ ] Existing widget-grid dashboard remains accessible as `/dashboard/classic`
- [ ] All new code has unit tests; full test suite passes
- [ ] Production TypeScript + Vite build succeeds

---

## Out of Scope (for this iteration)

- Real weather data (use placeholder)
- Multi-plant management (single-device focus, table lists existing devices)
- Historical data beyond existing `liveBufferAtom` range
- Actual export functionality (placeholder button only)
- Inverter Control / Delete actions (placeholder buttons)
