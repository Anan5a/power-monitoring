import { memo, useEffect, useMemo } from 'react'
import { useAtomValue, useSetAtom, useStore } from 'jotai'
import { SunIcon } from '@heroicons/react/24/outline'
import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  BarElement,
  Tooltip,
  type ChartOptions,
  type ChartData,
} from 'chart.js'
import { Bar } from 'react-chartjs-2'
import {
  generationDataAtom,
  generationLoadingAtom,
  generationRangeAtom,
  selectedDeviceAtom,
  type GenerationRange,
} from '../state/atoms'

ChartJS.register(CategoryScale, LinearScale, BarElement, Tooltip)

const RANGE_OPTIONS: { value: GenerationRange; label: string }[] = [
  { value: 'today', label: 'Today' },
  { value: 'yesterday', label: 'Yesterday' },
  { value: '7d', label: '7 Days' },
  { value: '30d', label: '30 Days' },
]

function GenerationWidget() {
  const store = useStore()
  const data = useAtomValue(generationDataAtom)
  const loading = useAtomValue(generationLoadingAtom)
  const range = useAtomValue(generationRangeAtom)
  const setRange = useSetAtom(generationRangeAtom)
  const device = useAtomValue(selectedDeviceAtom)

  // Re-run the generation fetcher whenever the selected range changes.
  // The fetcher atom is write-only and was only fired once in
  // startAggregatesPolling, so we have to drive it from the widget.
  useEffect(() => {
    if (!device) return
    let cancelled = false
    import('../state/services/aggregatesService').then(({ generationFetcherAtom }) => {
      if (!cancelled) store.set(generationFetcherAtom)
    })
    return () => { cancelled = true }
  }, [range, device, store])

  const chartData = useMemo<ChartData<'bar'>>(() => {
    const hourly = data?.hourly ?? []
    return {
      labels: hourly.map(h => h.hour),
      datasets: [{
        data: hourly.map(h => h.value),
        backgroundColor: hourly.map(h => h.projected ? 'rgba(251, 191, 36, 0.55)' : '#f59e0b'),
        borderRadius: 2,
        borderSkipped: false,
        barPercentage: 0.92,
        categoryPercentage: 0.95,
      }],
    }
  }, [data?.hourly])

  const chartOptions = useMemo<ChartOptions<'bar'>>(() => ({
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: 'rgb(30 41 59)',
        titleColor: '#cbd5e1',
        bodyColor: '#f1f5f9',
        padding: 8,
        cornerRadius: 6,
        displayColors: false,
        callbacks: {
          label: (ctx: any) => {
            const v = ctx.parsed.y
            const proj = data?.hourly?.[ctx.dataIndex]?.projected
            return `${v.toFixed(3)} kWh${proj ? ' (partial)' : ''}`
          },
        },
      },
    },
    scales: {
      x: {
        grid: { display: false },
        ticks: { color: '#94a3b8', font: { size: 9 }, maxRotation: 0, autoSkip: true, maxTicksLimit: 8 },
      },
      y: {
        display: false,
        beginAtZero: true,
      },
    },
  }), [data?.hourly])

  if (!device) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 p-4 flex items-center text-slate-300 text-sm">
        Select a device
      </div>
    )
  }

  const hasHourly = (data?.hourly?.length ?? 0) > 0

  return (
    <div className="h-full w-full bg-gradient-to-br from-amber-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-4 flex flex-col">
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <SunIcon className="w-5 h-5 text-amber-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Generation</span>
        </div>
        <span className="text-xs text-amber-600 font-medium">kWh</span>
      </div>
      <div className="flex items-center gap-1 mb-3 flex-wrap">
        {RANGE_OPTIONS.map(opt => (
          <button key={opt.value} onClick={() => setRange(opt.value)}
            className={`px-2.5 py-1 rounded-lg text-[11px] font-semibold transition-colors duration-150 ${range === opt.value ? 'bg-amber-500 text-white shadow-sm' : 'bg-slate-100 text-slate-500 hover:bg-slate-200'}`}>
            {opt.label}
          </button>
        ))}
      </div>
      <div className="flex items-baseline gap-1.5 mb-1 min-h-[36px]">
        {loading ? (
          <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
        ) : (
          <>
            <span className="text-2xl font-bold text-amber-600 tabular-nums">
              {data && data.total > 0 ? data.total.toFixed(2) : '0.00'}
            </span>
            <span className="text-sm font-medium text-amber-500">kWh</span>
          </>
        )}
      </div>
      <div className="text-[10px] text-slate-400 mb-2">{data?.rangeLabel ?? '—'}</div>
      <div className="flex-1 min-h-0">
        {hasHourly ? (
          <Bar data={chartData} options={chartOptions} />
        ) : (
          <div className="h-full flex items-center justify-center text-[10px] text-slate-300">
            {loading ? 'Loading…' : 'No generation data for this range'}
          </div>
        )}
      </div>
    </div>
  )
}

export default memo(GenerationWidget)
