import { memo, useEffect, useRef, useState } from 'react'
import { useAtomValue, useSetAtom } from 'jotai'
import { historyAtomFamily, drilldownLoadableAtom, type HistoryRange, type HistoryMetric } from '../state/history'
import { extractKeys, keyToLabel, suggestDrilldown, bucketToWindow } from '../state/services/historyService'
import { zoomRangeAtom, drilldownBreadcrumbAtom, hoveredPointAtom, deviceChannelsAtomFamily, latestAtom, refreshTriggerAtom } from '../state/atoms'
import HistoryTooltip from './HistoryTooltip'

// Re-export for tests
export { extractKeys } from '../state/services/historyService'

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
  const triggerRefresh = useSetAtom(refreshTriggerAtom)

  const breadcrumbRef = useRef(breadcrumb)
  const channelsRef = useRef(channels)
  useEffect(() => { breadcrumbRef.current = breadcrumb })
  useEffect(() => { channelsRef.current = channels })

  // Decide which loadable to use
  const drilldown = breadcrumb.length > 0 ? breadcrumb[breadcrumb.length - 1] : null
  const loadable: any = drilldown
    ? useAtomValue(drilldownLoadableAtom({ deviceKey, tStart: drilldown.tStart, tEnd: drilldown.tEnd, metric }))
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
  }, [loadable.state, metric])

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
          label: keyToLabel(k, channelsRef.current?.channel_names),
          stroke: SERIES_COLORS[i % SERIES_COLORS.length],
          width: 2,
          fill: SERIES_COLORS[i % SERIES_COLORS.length] + '40',
          points: { show: false },
        })
      })
      const fmtDate = (_u: any, splits: number[], _space: number) => {
        return splits.map(s => {
          const d = new Date(s * 1000)
          if (range === '1h' || range === '6h' || range === '24h') {
            return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
          }
          return d.toLocaleDateString([], { month: 'short', day: 'numeric' }) +
            ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
        })
      }
      const opts: any = {
        width: containerRef.current.clientWidth,
        height: 480,
        series,
        scales: { x: { time: true } },
        axes: [
          { stroke: '#94a3b8', grid: { stroke: '#f1f5f9' }, values: fmtDate },
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
      containerRef.current!.addEventListener('click', () => {
        const idx = plot.cursor.idx
        if (idx == null) return
        const t = new Date(xs[idx] * 1000)
        const bucketMs = range === '24h' ? 3600_000 : range === '7d' ? 86400_000 : range === '30d' ? 86400_000 : 3600_000
        const { tStart, tEnd } = bucketToWindow(t.toISOString(), bucketMs)
        const drilldownRange = suggestDrilldown(range, bucketMs)
        setBreadcrumb([...breadcrumbRef.current, { rangeLabel: `${range} → ${t.toLocaleDateString()}`, tStart: new Date(tStart).getTime(), tEnd: new Date(tEnd).getTime(), fromRange: drilldownRange }])
      })
      plotRef.current = plot
    })
    return () => {
      cancelled = true
      if (plotRef.current) { plotRef.current.destroy(); plotRef.current = null }
    }
  }, [loadable.state, range, metric, visibleLines.size, visibleLines])

  // Push live data point into uPlot on every latest update
  useEffect(() => {
    if (!plotRef.current || !latest) return
    const t = new Date(latest.recorded_at).getTime() / 1000
    if (t <= seriesDataRef.current.xs[seriesDataRef.current.xs.length - 1]) return
    const newXs = [...seriesDataRef.current.xs, t]
    const newYsList = seriesDataRef.current.ysList.map(ys => {
      const idx = seriesDataRef.current.ysList.indexOf(ys)
      const key = seriesDataRef.current.keys[idx]
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