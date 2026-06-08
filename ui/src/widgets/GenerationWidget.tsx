import { memo } from 'react'
import { useAtomValue, useSetAtom } from 'jotai'
import { SunIcon } from '@heroicons/react/24/outline'
import {
  generationDataAtom,
  generationLoadingAtom,
  generationRangeAtom,
  selectedDeviceAtom,
  type GenerationRange,
} from '../state/atoms'

const RANGE_OPTIONS: { value: GenerationRange; label: string }[] = [
  { value: 'today', label: 'Today' },
  { value: 'yesterday', label: 'Yesterday' },
  { value: '7d', label: '7 Days' },
  { value: '30d', label: '30 Days' },
]

function GenerationWidget() {
  const data = useAtomValue(generationDataAtom)
  const loading = useAtomValue(generationLoadingAtom)
  const range = useAtomValue(generationRangeAtom)
  const setRange = useSetAtom(generationRangeAtom)
  const device = useAtomValue(selectedDeviceAtom)

  if (!device) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 p-4 flex items-center text-slate-300 text-sm">
        Select a device
      </div>
    )
  }

  return (
    <div className="h-full w-full bg-gradient-to-br from-amber-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-4">
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
      <div className="text-[10px] text-slate-400">{data?.rangeLabel ?? '—'}</div>
    </div>
  )
}

export default memo(GenerationWidget)
