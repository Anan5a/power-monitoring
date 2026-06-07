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
