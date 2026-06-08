import { memo, useEffect, useRef, useState, useMemo, useCallback } from 'react'
import { useAtomValue, useSetAtom } from 'jotai'
import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  TimeScale,
  PointElement,
  LineElement,
  LineController,
  Filler,
  Tooltip,
  Legend as ChartLegend,
  type ChartOptions,
  type ChartData,
} from 'chart.js'
import 'chartjs-adapter-date-fns'
import zoomPlugin from 'chartjs-plugin-zoom'
import { Line } from 'react-chartjs-2'
import {
  historyAtomFamily,
  drilldownLoadableAtom,
  type HistoryRange,
  type HistoryMetric,
} from '../state/history'
import { extractKeys, keyToLabel, suggestDrilldown, bucketToWindow, buildChannelLabelMap } from '../state/services/historyService'
import {
  zoomRangeAtom,
  drilldownBreadcrumbAtom,
  hoveredPointAtom,
  deviceChannelsAtomFamily,
  latestAtom,
  refreshTriggerAtom,
} from '../state/atoms'
// Chart.js built-in tooltip handles hover; custom HTML tooltip removed.

// Re-export for tests
export { extractKeys } from '../state/services/historyService'

ChartJS.register(
  CategoryScale,
  LinearScale,
  TimeScale,
  PointElement,
  LineElement,
  LineController,
  Filler,
  Tooltip,
  ChartLegend,
  zoomPlugin,
)

const DOWNSAMPLE_TARGET = 1500

interface Props {
  deviceKey: string
}

// Yellow / orange family for PV across all metrics. Other series get distinct hues.
const PV_COLORS = {
  power:    '#f59e0b', // amber
  voltage:  '#fbbf24', // amber-400
  current:  '#fb923c', // orange-400
}
const PALETTE_FALLBACK = [
  '#3b82f6', // blue
  '#10b981', // emerald
  '#a855f7', // purple
  '#06b6d4', // cyan
  '#ec4899', // pink
  '#84cc16', // lime
  '#ef4444', // red
  '#6366f1', // indigo
  '#14b8a6', // teal
  '#f43f5e', // rose
]

function colorForKey(k: string, metric: 'power' | 'voltage' | 'current', index: number): string {
  if (k === 'pv_power') return PV_COLORS.power
  if (k === 'ina226_p' || k === 'ina226_v' || k === 'ina226_i') return '#a855f7' // purple for the INA226 standalone
  if (k === 'inverter_power' || k === 'inverter_current') return '#3b82f6' // blue for inverter
  if (k === 'battery_power') return '#10b981' // emerald for battery
  if (k === 'dc_load_power') return '#ef4444' // red for DC load
  if (k === 'soc_pct0') return '#a855f7' // purple for SoC
  // For raw channel keys (ch0_V, ch0_P, etc.), the first one is usually PV.
  // We can't tell PV from a generic chN_V without channel_names, so use PV color
  // for the first voltage/current series on the corresponding tab and fall back.
  if (/^ch\d_p$/i.test(k) && metric === 'power') {
    return index === 0 ? PV_COLORS.power : PALETTE_FALLBACK[(index - 1) % PALETTE_FALLBACK.length]
  }
  if (/^ch\d_v$/i.test(k) && metric === 'voltage') {
    return index === 0 ? PV_COLORS.voltage : PALETTE_FALLBACK[(index - 1) % PALETTE_FALLBACK.length]
  }
  if (/^ch\d_i$/i.test(k) && metric === 'current') {
    return index === 0 ? PV_COLORS.current : PALETTE_FALLBACK[(index - 1) % PALETTE_FALLBACK.length]
  }
  return PALETTE_FALLBACK[index % PALETTE_FALLBACK.length]
}

const UNIT: Record<HistoryMetric, string> = { power: 'W', voltage: 'V', current: 'A' }

function fmtTick(iso: string, range: HistoryRange): string {
  const d = new Date(iso)
  if (range === '1h' || range === '6h' || range === '24h') {
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  }
  return d.toLocaleDateString([], { month: 'short', day: 'numeric' }) +
    ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

// Mouse-move bridge: feed hoveredPointAtom so the tooltip can render
// outside the canvas. Uses a ref so we don't add a per-mousemove subscription.
function makeHoverSyncPlugin(setHoveredRef: { current: (p: { time: string; values: Record<string, number> } | null) => void }) {
  return {
    id: 'hoverSync',
    afterEvent(chart: any) {
      const active = chart.tooltip?.getActiveElements()
      if (!active || active.length === 0) {
        setHoveredRef.current(null)
        return
      }
      const idx = active[0].index
      const ds = chart.data.datasets
      const values: Record<string, number> = {}
      for (let i = 0; i < ds.length; i++) {
        const v = (ds[i].data as any[])[idx]
        const label = ds[i].label
        if (typeof v === 'number' && label) values[label] = v
      }
      const t = (chart.data.datasets[0].data as any[])[idx]
      setHoveredRef.current({
        time: typeof t === 'number' ? new Date(t).toLocaleString() : String(t),
        values,
      })
    },
  } as any
}

function HistoryChartWidget({ deviceKey }: Props) {
  const [range, setRange] = useState<HistoryRange>('24h')
  const [metric, setMetric] = useState<HistoryMetric>('power')
  const [hiddenKeys, setHiddenKeys] = useState<Set<string>>(new Set())
  // Source of truth for the chart: state, not a ref. The build effect writes
  // here; the live-append effect mutates via setState so React re-renders.
  const [series, setSeries] = useState<{ points: { t: number; v: Record<string, number> }[]; keys: string[] }>({ points: [], keys: [] })
  const chartRef = useRef<any>(null)

  const breadcrumb = useAtomValue(drilldownBreadcrumbAtom)
  const setBreadcrumb = useSetAtom(drilldownBreadcrumbAtom)
  const zoom = useAtomValue(zoomRangeAtom)
  const setZoom = useSetAtom(zoomRangeAtom)
  const setHovered = useSetAtom(hoveredPointAtom)
  const setHoveredRef = useRef(setHovered)
  useEffect(() => { setHoveredRef.current = setHovered })
  const hoverSyncPlugin = useMemo(() => makeHoverSyncPlugin(setHoveredRef), [])
  const channels = useAtomValue(deviceChannelsAtomFamily(deviceKey))
  const channelLabelMap = useMemo(() => buildChannelLabelMap(channels?.channel_groups), [channels?.channel_groups])
  const latest = useAtomValue(latestAtom)
  const triggerRefresh = useSetAtom(refreshTriggerAtom)

  const breadcrumbRef = useRef(breadcrumb)
  useEffect(() => { breadcrumbRef.current = breadcrumb })

  const drilldown = breadcrumb.length > 0 ? breadcrumb[breadcrumb.length - 1] : null
  const loadable: any = drilldown
    ? useAtomValue(drilldownLoadableAtom({ deviceKey, tStart: drilldown.tStart, tEnd: drilldown.tEnd, metric }))
    : useAtomValue(historyAtomFamily({ deviceKey, range, metric }))

  // Build the time series from RPC output
  useEffect(() => {
    if (loadable.state !== 'hasData') {
      setSeries({ points: [], keys: [] })
      return
    }
    const data = loadable.data as any[]
    if (data.length === 0) {
      setSeries({ points: [], keys: [] })
      return
    }
    const keys = extractKeys(data, metric)
    if (keys.length === 0) {
      setSeries({ points: [], keys: [] })
      return
    }
    const step = data.length > DOWNSAMPLE_TARGET ? Math.ceil(data.length / DOWNSAMPLE_TARGET) : 1
    const points: { t: number; v: Record<string, number> }[] = []
    for (let i = 0; i < data.length; i += step) {
      const pt = data[i]
      const t = new Date(pt.recorded_at).getTime()
      const v: Record<string, number> = {}
      for (const k of keys) v[k] = (pt.payload as any)[k] ?? null
      points.push({ t, v })
    }
    setSeries({ points, keys })
  }, [loadable.state, loadable.data, metric])

  // Live-append latest sample, then setSeries re-renders the chart
  useEffect(() => {
    if (!latest) return
    const t = new Date(latest.recorded_at).getTime()
    const last = series.points[series.points.length - 1]
    if (last && t <= last.t) return
    if (series.keys.length === 0) return
    const v: Record<string, number> = {}
    for (const k of series.keys) v[k] = (latest.payload as any)[k] ?? null
    setSeries(s => ({ ...s, points: [...s.points, { t, v }] }))
  }, [latest])

  const onChartClick = useCallback((_e: any, _els: any, chart: any) => {
    if (!chart?.tooltip) return
    const active = chart.tooltip.getActiveElements()
    if (active.length === 0) return
    const idx = active[0].index
    const point = series.points[idx]
    if (!point) return
    const bucketMs = range === '24h' ? 3600_000 : range === '7d' ? 86400_000 : range === '30d' ? 86400_000 : 3600_000
    const tISO = new Date(point.t).toISOString()
    const { tStart, tEnd } = bucketToWindow(tISO, bucketMs)
    const drilldownRange = suggestDrilldown(range, bucketMs)
    const dateLabel = new Date(point.t).toLocaleDateString()
    setBreadcrumb([
      ...breadcrumbRef.current,
      { rangeLabel: `${range} → ${dateLabel}`, tStart: new Date(tStart).getTime(), tEnd: new Date(tEnd).getTime(), fromRange: drilldownRange },
    ])
  }, [range, setBreadcrumb])

  // Split datasets by axis: power keys go to the left y axis, soc_pct* goes to the right.
  const chartData = useMemo<ChartData<'line'>>(() => {
    const { points, keys } = series
    return {
      datasets: keys.map((k, i) => {
        const isSoc = k.toLowerCase().startsWith('soc_pct')
        return {
          label: keyToLabel(k, channels?.channel_names, channelLabelMap),
          data: points.map(p => ({ x: p.t, y: p.v[k] ?? null })),
          borderColor: colorForKey(k, metric, i),
          backgroundColor: colorForKey(k, metric, i) + '40',
          borderWidth: 2,
          fill: !isSoc && metric === 'power' ? 'origin' : false,
          pointRadius: 0,
          pointHoverRadius: 4,
          tension: 0.3,
          hidden: hiddenKeys.has(k),
          yAxisID: isSoc ? 'y1' : 'y',
          borderDash: isSoc ? [4, 4] : undefined,
        }
      }),
    }
  }, [series, channels, metric, hiddenKeys])

  const showSecondaryAxis = metric === 'power'

  const chartOptions = useMemo<ChartOptions<'line'>>(() => ({
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    parsing: false,
    spanGaps: true,
    interaction: { mode: 'index', intersect: false },
    onClick: onChartClick,
    scales: {
      x: {
        type: 'time' as const,
        time: {
          unit: range === '1h' ? 'hour' : range === '6h' ? 'hour' : range === '24h' ? 'hour' : 'day',
          displayFormats: { hour: 'HH:mm', day: 'MMM d' },
        },
        grid: { color: '#f1f5f9' },
        ticks: { color: '#94a3b8', maxRotation: 0, autoSkip: true, maxTicksLimit: 8 },
      },
      y: {
        position: 'left' as const,
        grid: { color: '#f1f5f9' },
        ticks: {
          color: '#94a3b8',
          callback: (v: any) => `${v} ${UNIT[metric]}`,
        },
        title: {
          display: true,
          text: `${metric.charAt(0).toUpperCase() + metric.slice(1)} (${UNIT[metric]})`,
          color: '#94a3b8',
        },
      },
      y1: showSecondaryAxis ? {
        position: 'right' as const,
        grid: { drawOnChartArea: false },
        ticks: { color: '#a855f7' },
        min: 0,
        max: 100,
        title: { display: true, text: 'SoC (%)', color: '#a855f7' },
      } : undefined,
    },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: 'rgb(30 41 59)',
        titleColor: '#cbd5e1',
        bodyColor: '#f1f5f9',
        padding: 10,
        cornerRadius: 8,
        displayColors: false,
        callbacks: {
          title: (items: any[]) => items.length ? fmtTick(new Date(items[0].parsed.x).toISOString(), range) : '',
          label: (ctx: any) => {
            const v = ctx.parsed.y
            if (v == null) return ''
            const isSoc = (ctx.dataset.label ?? '').toLowerCase().includes('soc')
            if (isSoc) return `${ctx.dataset.label}: ${v.toFixed(0)} %`
            const decimals = metric === 'voltage' ? 2 : 1
            return `${ctx.dataset.label}: ${Math.abs(v).toFixed(decimals)} ${UNIT[metric]}`
          },
        },
      },
      zoom: {
        pan: { enabled: true, mode: 'x' as const },
        zoom: {
          wheel: { enabled: true },
          drag: { enabled: true, backgroundColor: 'rgba(59, 130, 246, 0.1)' },
          mode: 'x' as const,
          onZoomComplete: ({ chart }: any) => {
            const xScale = chart.scales.x
            setZoom({ start: xScale.min, end: xScale.max })
          },
          onPanComplete: ({ chart }: any) => {
            const xScale = chart.scales.x
            setZoom({ start: xScale.min, end: xScale.max })
          },
        },
      },
    },
  }), [range, metric, setZoom, onChartClick, showSecondaryAxis])

  // visibleKeys removed (was only used by the deleted custom tooltip)

  if (loadable.state === 'loading') {
    return <div className="h-full w-full bg-slate-50 animate-pulse rounded-2xl" />
  }
  if (loadable.state === 'hasError') {
    return <div className="h-full w-full bg-red-50 text-red-600 rounded-2xl p-4">Failed to load: {String((loadable as any).error)}</div>
  }

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 p-4">
      <div className="flex items-center justify-between mb-3 flex-wrap gap-2">
        <h3 className="font-bold text-slate-800 text-sm">History</h3>
        <div className="flex items-center gap-1 flex-wrap">
          {(['1h', '6h', '24h', '7d', '30d'] as HistoryRange[]).map(r => (
            <button key={r} onClick={() => { setRange(r); setBreadcrumb([]); setZoom(null); setHiddenKeys(new Set()) }}
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
            <button onClick={() => { setZoom(null); if (chartRef.current) chartRef.current.resetZoom() }}
              className="ml-2 px-2 py-0.5 rounded bg-slate-100 text-slate-500 text-[11px]">
              Reset zoom
            </button>
          )}
          <button onClick={() => triggerRefresh(n => n + 1)} className="ml-1 px-2 py-0.5 rounded bg-slate-100 text-slate-500 text-[11px]">↻</button>
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
        {series.keys.map((k, i) => {
          const active = !hiddenKeys.has(k)
          const color = colorForKey(k, metric, i)
          return (
            <button key={k} onClick={() => {
              const next = new Set(hiddenKeys)
              if (next.has(k)) next.delete(k); else next.add(k)
              setHiddenKeys(next)
            }}
              className={`flex items-center gap-1.5 text-[11px] px-2 py-0.5 rounded-full border transition-all duration-150 ${active ? 'border-transparent shadow-sm' : 'border-slate-200 text-slate-400 opacity-60'}`}
              style={active ? { backgroundColor: color + '18', borderColor: color + '40' } : {}}>
              <span className="w-2.5 h-2.5 rounded-full flex-shrink-0" style={{ background: color }} />
              <span style={active ? { color } : {}}>{keyToLabel(k, channels?.channel_names, channelLabelMap)}</span>
            </button>
          )
        })}
      </div>
      <div className="relative" style={{ height: 480 }}>
        <Line
          ref={chartRef as any}
          data={chartData}
          options={chartOptions}
          plugins={[hoverSyncPlugin]}
        />
          // Tooltip rendered by Chart.js; see chartOptions.plugins.tooltip.
      </div>
    </div>
  )
}

export default memo(HistoryChartWidget)
