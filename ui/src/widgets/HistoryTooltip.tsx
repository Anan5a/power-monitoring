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