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
  historyStreamAtomFamily,
  startHistoryStreamAtom,
  drilldownStreamAtomFamily,
  startDrilldownStreamAtom,
  RANGE_HOURS,
  type HistoryRange,
  type HistoryMetric,
  type HistoryStreamState,
} from '../state/history'
import { extractKeys, keyToLabel, suggestDrilldown, bucketToWindow, buildChannelLabelMap, withSystemCurrents, addGridPower, SYSTEM_CURRENT_KEYS } from '../state/services/historyService'
import {
  zoomRangeAtom,
  drilldownBreadcrumbAtom,
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
  if (k === 'pv_power' || k === 'pv_current') return metric === 'current' ? PV_COLORS.current : PV_COLORS.power
  if (k === 'ina226_p' || k === 'ina226_v' || k === 'ina226_i') return '#a855f7' // purple for the INA226 standalone
  if (k === 'inverter_power' || k === 'inverter_current') return '#3b82f6' // blue for inverter
  if (k === 'battery_power' || k === 'battery_current') return '#10b981' // emerald for battery
  if (k === 'dc_load_power' || k === 'dc_load_current') return '#ef4444' // red for DC load
  if (k === 'grid_power') return '#f43f5e' // rose for grid
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

function fmtTick(iso: string, showDate: boolean): string {
  const d = new Date(iso)
  if (!showDate) {
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  }
  return d.toLocaleDateString([], { month: 'short', day: 'numeric' }) +
    ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
}

function spanToTimeUnit(ms: number): 'minute' | 'hour' | 'day' {
  if (ms < 60 * 60 * 1000) return 'minute'
  if (ms < 24 * 60 * 60 * 1000) return 'hour'
  return 'day'
}

// Threshold for triggering a raw-data drilldown when zooming in.
const DRILLDOWN_SPAN_MS = 2 * 60 * 60 * 1000
// Only drill down when the visible window is at least this much smaller
// than the currently-loaded window (prevents tiny zoom jitters).
const DRILLDOWN_ZOOM_RATIO = 2.5

function HistoryChartWidget({ deviceKey }: Props) {
  const [range, setRange] = useState<HistoryRange>('6h')
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
  const channels = useAtomValue(deviceChannelsAtomFamily(deviceKey))
  const channelLabelMap = useMemo(() => buildChannelLabelMap(channels?.channel_groups), [channels?.channel_groups])
  const latest = useAtomValue(latestAtom)
  const triggerRefresh = useSetAtom(refreshTriggerAtom)

  const breadcrumbRef = useRef(breadcrumb)
  useEffect(() => { breadcrumbRef.current = breadcrumb })

  const drilldown = breadcrumb.length > 0 ? breadcrumb[breadcrumb.length - 1] : null
  const streamState: HistoryStreamState = drilldown
    ? useAtomValue(drilldownStreamAtomFamily({ deviceKey, tStart: drilldown.tStart, tEnd: drilldown.tEnd, metric }))
    : useAtomValue(historyStreamAtomFamily({ deviceKey, range, metric }))
  const startStream = useSetAtom(drilldown ? startDrilldownStreamAtom : startHistoryStreamAtom)

  // Trigger fetch when params change; re-fetch on manual refresh
  const trigger = useAtomValue(refreshTriggerAtom)
  useEffect(() => {
    if (drilldown) {
      startStream({ deviceKey, tStart: drilldown.tStart, tEnd: drilldown.tEnd, metric } as any)
    } else {
      startStream({ deviceKey, range, metric } as any)
    }
  }, [deviceKey, range, metric, drilldown, startStream, trigger])

  // Build the time series from streamed data (renders incrementally as pages arrive)
  useEffect(() => {
    const data = streamState.data
    if (data.length === 0) {
      setSeries({ points: [], keys: [] })
      return
    }
    const channelGroups = channels?.channel_groups
    let keys = extractKeys(data, metric, channelGroups)
    // On the Current tab, surface only the four system currents. The raw
    // ch*_I / ina226_i series are aggregated into pv_current,
    // battery_current, inverter_current, dc_load_current so showing them
    // too would double the lines and double the legend.
    if (metric === 'current') {
      keys = keys.filter(k => SYSTEM_CURRENT_KEYS.includes(k))
    }
    if (keys.length === 0) {
      setSeries({ points: [], keys: [] })
      return
    }
    const step = data.length > DOWNSAMPLE_TARGET ? Math.ceil(data.length / DOWNSAMPLE_TARGET) : 1
    const points: { t: number; v: Record<string, number> }[] = []
    for (let i = 0; i < data.length; i += step) {
      const pt = data[i]
      const t = new Date(pt.recorded_at).getTime()
      // Augment the payload with derived system current keys so they survive
      // the per-point `keys.map(...)` lookup below.
      const payload = metric === 'current'
        ? withSystemCurrents(pt.payload as Record<string, number>, channelGroups)
        : metric === 'power'
          ? addGridPower(pt.payload as Record<string, number>)
          : (pt.payload as Record<string, number>)
      const v: Record<string, number> = {}
      for (const k of keys) v[k] = payload[k] ?? null
      points.push({ t, v })
    }
    setSeries({ points, keys })
  }, [streamState.data, metric, channels?.channel_groups])

  // Live-append latest sample, then setSeries re-renders the chart
  useEffect(() => {
    if (!latest) return
    const t = new Date(latest.recorded_at).getTime()
    const last = series.points[series.points.length - 1]
    if (last && t <= last.t) return
    if (series.keys.length === 0) return
    const payload = metric === 'current'
      ? withSystemCurrents(latest.payload as Record<string, number>, channels?.channel_groups)
      : metric === 'power'
        ? addGridPower(latest.payload as Record<string, number>)
        : (latest.payload as Record<string, number>)
    const v: Record<string, number> = {}
    for (const k of series.keys) v[k] = payload[k] ?? null
    setSeries(s => ({ ...s, points: [...s.points, { t, v }] }))
  }, [latest])

  // Re-apply the saved zoom range whenever the underlying data changes.
  // Without this, new data (live-append, metric switch, range change) resets
  // the x-axis to the full extent and discards the user's current view.
  useEffect(() => {
    if (!zoom || !chartRef.current) return
    const chart = chartRef.current
    const apply = () => {
      const xScale = chart.scales?.x
      if (!xScale) return
      // Only re-apply if the saved range is still within the new data extent.
      const min = Math.max(zoom.start, xScale.min)
      const max = Math.min(zoom.end, xScale.max)
      if (min >= max) return
      chart.zoomScale('x', { min, max }, 'none')
    }
    // The chart may not have re-rendered the new data yet; defer one frame.
    const id = requestAnimationFrame(apply)
    return () => cancelAnimationFrame(id)
  }, [series, zoom])

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

  // Zoom / pan into a narrow window should load raw, higher-resolution data
  // for that exact window via the drilldown fetcher.
  const maybeTriggerDrilldown = useCallback((start: number, end: number) => {
    const visibleMs = Math.max(0, end - start)
    const loadedStart = drilldown ? drilldown.tStart : Date.now() - RANGE_HOURS[range] * 3600 * 1000
    const loadedEnd = drilldown ? drilldown.tEnd : Date.now()
    const loadedMs = Math.max(1, loadedEnd - loadedStart)
    // No need to drill down when the base range is already raw 1-sec data.
    if (range === '1h') return
    const shouldDrilldown = visibleMs <= DRILLDOWN_SPAN_MS
      && visibleMs * DRILLDOWN_ZOOM_RATIO < loadedMs
    if (!shouldDrilldown) return
    setBreadcrumb([
      ...breadcrumbRef.current,
      {
        rangeLabel: 'zoomed',
        tStart: start,
        tEnd: end,
        fromRange: '1h',
      },
    ])
  }, [drilldown, range, setBreadcrumb])

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
          fill: !isSoc && (metric === 'power' || metric === 'current') ? 'origin' : false,
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

  // X-axis granularity should follow the actual visible span, not the top-level
  // range, so drilldowns and zoom windows get minute-level ticks when needed.
  const xAxisUnit = useMemo(() => {
    const span = zoom
      ? zoom.end - zoom.start
      : drilldown
        ? drilldown.tEnd - drilldown.tStart
        : RANGE_HOURS[range] * 3600 * 1000
    return spanToTimeUnit(span)
  }, [zoom, drilldown, range])

  const chartOptions = useMemo<ChartOptions<'line'>>(() => ({
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    parsing: false,
    spanGaps: true,
    interaction: { mode: 'index', intersect: false },
    onClick: onChartClick,
    layout: {
      padding: { top: 4, bottom: 0, left: 0, right: 0 },
    },
    scales: {
      x: {
        type: 'time' as const,
        time: {
          unit: xAxisUnit,
          displayFormats: { minute: 'HH:mm', hour: 'HH:mm', day: 'MMM d' },
        },
        grid: { color: '#f1f5f9' },
        ticks: { color: '#94a3b8', maxRotation: 0, autoSkip: true, maxTicksLimit: 8 },
      },
      y: {
        type: 'linear' as const,
        position: 'left' as const,
        grid: { color: '#f1f5f9' },
        ticks: {
          color: '#94a3b8',
          font: { size: 10 },
          callback: (v: any) => `${v} ${UNIT[metric]}`,
        },
        title: {
          display: true,
          text: `${metric.charAt(0).toUpperCase() + metric.slice(1)} (${UNIT[metric]})`,
          color: '#94a3b8',
          font: { size: 10 },
        },
      },
      // Always define y1 to keep the scale registered with Chart.js, but hide
      // it when not in use. Flipping between defined/undefined triggers the
      // "Invalid scale configuration for scale: y1" error on tab switch.
      y1: {
        type: 'linear' as const,
        position: 'right' as const,
        display: showSecondaryAxis,
        grid: { drawOnChartArea: false },
        ticks: { color: '#a855f7', font: { size: 10 } },
        min: 0,
        max: 100,
        title: { display: showSecondaryAxis, text: 'SoC (%)', color: '#a855f7', font: { size: 10 } },
      },
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
          title: (items: any[]) => items.length ? fmtTick(new Date(items[0].parsed.x).toISOString(), xAxisUnit === 'day') : '',
          label: (ctx: any) => {
            const v = ctx.parsed.y
            if (v == null) return ''
            const isSoc = (ctx.dataset.label ?? '').toLowerCase().includes('soc')
            if (isSoc) return `${ctx.dataset.label}: ${v.toFixed(0)} %`
            const decimals = metric === 'voltage' ? 2 : 1
            // On the Current tab, system currents carry a meaningful sign
            // (battery charging is +, discharging is -; inverter flow
            // direction; PV / DC load are always >= 0). Show the sign.
            // On the Power tab we keep the existing convention of stripping
            // the sign — the four power series are all net values where the
            // user is more interested in magnitude than flow direction.
            const showSign = metric === 'current'
            const valueStr = (showSign && v < 0 ? '-' : '') + Math.abs(v).toFixed(decimals)
            return `${ctx.dataset.label}: ${valueStr} ${UNIT[metric]}`
          },
        },
      },
      zoom: {
        pan: {
          enabled: true,
          mode: 'x' as const,
          modifierKey: 'shift' as const,
          onPanComplete: ({ chart }: any) => {
            const xScale = chart.scales.x
            setZoom({ start: xScale.min, end: xScale.max })
          },
        },
        zoom: {
          wheel: { enabled: true, speed: 0.15 },
          // Drag-to-zoom: normal drag selects a region to zoom into.
          // Shift+drag still pans. This avoids the two gestures conflicting.
          drag: { enabled: true, backgroundColor: 'rgba(59, 130, 246, 0.15)' },
          mode: 'x' as const,
          onZoomComplete: ({ chart }: any) => {
            const xScale = chart.scales.x
            setZoom({ start: xScale.min, end: xScale.max })
            maybeTriggerDrilldown(xScale.min, xScale.max)
          },
        },
      },
    },
  }), [range, metric, setZoom, onChartClick, showSecondaryAxis, xAxisUnit, maybeTriggerDrilldown])

  // visibleKeys removed (was only used by the deleted custom tooltip)

  const isLoading = streamState.loading && streamState.data.length === 0
  const error = streamState.error

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 p-2 sm:p-4">
      <div className="flex items-center justify-between mb-3 flex-wrap gap-2">
        <h3 className="font-bold text-slate-800 text-sm">History</h3>
        <div className="flex items-center gap-1 flex-wrap">
          {(['1h', '6h', '24h', '7d', '30d'] as HistoryRange[]).map(r => (
            <button key={r} onClick={() => { setRange(r); setBreadcrumb([]); setZoom(null); setHiddenKeys(new Set()) }}
              className={`px-2.5 py-1 rounded-lg text-[11px] font-semibold transition-colors duration-150 ${range === r ? 'bg-gradient-to-r from-cyan-500 to-blue-500 text-white shadow-sm' : 'bg-slate-100 text-slate-500 hover:bg-slate-200'}`}>
              {r}
            </button>
          ))}
          {(breadcrumb.length > 0 || zoom) && (
            <div className="flex items-center gap-1 ml-2 text-[11px]">
              {breadcrumb.length > 0 && (
                <button
                  onClick={() => {
                    const next = breadcrumb.slice(0, -1)
                    setBreadcrumb(next)
                    setZoom(null)
                    if (chartRef.current) chartRef.current.resetZoom()
                  }}
                  className="px-2 py-0.5 rounded bg-slate-100 hover:bg-slate-200 text-slate-600"
                >
                  ← Back
                </button>
              )}
              {zoom && (
                <button
                  onClick={() => { setZoom(null); if (chartRef.current) chartRef.current.resetZoom() }}
                  className="px-2 py-0.5 rounded bg-slate-100 hover:bg-slate-200 text-slate-600"
                >
                  Reset
                </button>
              )}
            </div>
          )}
          <button onClick={() => triggerRefresh(n => n + 1)} className="ml-1 px-2 py-0.5 rounded bg-slate-100 text-slate-500 text-[11px]">↻</button>
        </div>
      </div>
      <div className="flex items-center gap-1.5 mb-3">
        {(['power', 'voltage', 'current'] as HistoryMetric[]).map(m => (
          <button key={m} onClick={() => { setMetric(m); setHiddenKeys(new Set()) }}
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
        {series.keys.length > 0 && (() => {
          const allHidden = hiddenKeys.size === series.keys.length
          return (
            <button
              onClick={() => setHiddenKeys(allHidden ? new Set() : new Set(series.keys))}
              className="ml-auto px-2 py-0.5 rounded text-[11px] font-medium bg-slate-100 text-slate-500 hover:bg-slate-200 transition-colors duration-150"
            >
              {allHidden ? 'Show all' : 'Hide all'}
            </button>
          )
        })()}
      </div>
      <div className="relative h-[60vh] sm:h-[480px]">
        {isLoading && (
          <div className="absolute inset-0 z-10 bg-white/70 rounded-2xl flex items-center justify-center">
            <div className="w-8 h-8 border-2 border-slate-300 border-t-brand-500 rounded-full animate-spin" />
          </div>
        )}
        {error && (
          <div className="absolute inset-0 z-10 bg-red-50 text-red-600 rounded-2xl p-4 flex items-center justify-center text-sm">
            Failed to load: {error}
          </div>
        )}
        <Line
          ref={chartRef as any}
          data={chartData}
          options={chartOptions}
        />
      </div>
    </div>
  )
}

export default memo(HistoryChartWidget)
