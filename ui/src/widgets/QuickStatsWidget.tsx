import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { SunIcon, ArrowUpIcon, ArrowDownIcon } from '@heroicons/react/24/outline'
import { computedTelemetryAtom, pvPowerAtom, batteryPowerAtom, inverterPowerAtom, systemStatusAtom } from '../state/derived'
import { latestAtom } from '../state/atoms'

const STATUS_STYLES: Record<string, { dot: string; text: string; label: string }> = {
  charging:    { dot: 'bg-emerald-500', text: 'text-emerald-700', label: 'Charging' },
  discharging: { dot: 'bg-amber-500',   text: 'text-amber-700',   label: 'Discharging' },
  balanced:    { dot: 'bg-sky-500',     text: 'text-sky-700',     label: 'Balanced' },
  unknown:     { dot: 'bg-slate-400',   text: 'text-slate-600',   label: 'Unknown' },
}

function StaticNumber({ value, suffix = '', decimals = 1 }: { value: number; suffix?: string; decimals?: number }) {
  const display = Math.abs(value) < 0.05 && value !== 0 ? '0' : value.toFixed(decimals)
  return <span className="tabular-nums">{display}{suffix}</span>
}

function Chip({ label, value, unit, color, icon }: { label: string; value: number; unit: string; color: string; icon?: React.ReactNode }) {
  return (
    <div className="flex items-center gap-2.5 min-w-fit">
      {icon && <div className={`shrink-0 ${color}`}>{icon}</div>}
      <div className="flex flex-col leading-tight">
        <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">{label}</span>
        <span className={`text-lg font-semibold tabular-nums transition-colors duration-200 ${color}`}>
          <StaticNumber value={value} />
          <span className="text-sm font-normal text-slate-400 ml-0.5">{unit}</span>
        </span>
      </div>
    </div>
  )
}

function Directional({ label, value, unit }: { label: string; value: number; unit: string }) {
  const positive = value > 0.5
  const negative = value < -0.5
  const color = positive ? 'text-emerald-600' : negative ? 'text-cyan-600' : 'text-slate-400'
  const Icon = positive ? ArrowUpIcon : negative ? ArrowDownIcon : null
  return (
    <div className="flex items-center gap-2.5 min-w-fit">
      <div className="shrink-0 w-5 h-5">
        {Icon && <Icon className={`w-5 h-5 ${color}`} />}
      </div>
      <div className="flex flex-col leading-tight">
        <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">{label}</span>
        <span className={`text-lg font-semibold tabular-nums transition-colors duration-200 ${color}`}>
          <StaticNumber value={Math.abs(value)} />
          <span className="text-sm font-normal text-slate-400 ml-0.5">{unit}</span>
        </span>
      </div>
    </div>
  )
}

function QuickStatsWidget() {
  const computed = useAtomValue(computedTelemetryAtom)
  const pv = useAtomValue(pvPowerAtom)
  const battery = useAtomValue(batteryPowerAtom)
  const inverter = useAtomValue(inverterPowerAtom)
  const status = useAtomValue(systemStatusAtom)
  const latest = useAtomValue(latestAtom)
  const totalPower = Math.abs(inverter) + computed.dc_load_power
  const style = STATUS_STYLES[status]

  if (!latest) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-6 flex items-center text-slate-300 text-sm">
        Awaiting telemetry…
      </div>
    )
  }

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-6 flex items-center overflow-hidden">
      <div className="flex items-center gap-6 min-w-fit">
        <div className="flex flex-col leading-tight">
          <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">Total Power</span>
          <span className="text-2xl font-bold text-slate-800 tabular-nums">
            {totalPower > 0 ? <><StaticNumber value={totalPower} /><span className="text-base font-normal text-slate-400 ml-1">W</span></> : <span className="text-slate-300">--</span>}
          </span>
        </div>
        <div className="h-10 w-px bg-slate-200" />
        <Chip label="PV" value={pv} unit="W" color="text-amber-500" icon={<SunIcon className="w-5 h-5" />} />
        <div className="h-10 w-px bg-slate-200" />
        <Directional label="Inverter" value={inverter} unit="W" />
        <div className="h-10 w-px bg-slate-200" />
        <Directional label="Battery" value={battery} unit="W" />
      </div>
      <div className="ml-auto flex items-center gap-3 shrink-0">
        <div className="flex items-center gap-2.5 px-3.5 py-1.5 rounded-full bg-slate-50">
          <span className={`w-3 h-3 rounded-full ${style.dot} ${status !== 'unknown' && status !== 'balanced' ? 'animate-pulse' : ''}`} />
          <span className={`text-xs font-semibold ${style.text}`}>{style.label}</span>
        </div>
      </div>
    </div>
  )
}

export default memo(QuickStatsWidget)
