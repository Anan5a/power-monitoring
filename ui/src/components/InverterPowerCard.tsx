import { motion, AnimatePresence } from 'framer-motion'
import { ArrowUpIcon, ArrowDownIcon } from '@heroicons/react/24/outline'

interface Props {
  inverterPower: number
  systemStatus: string
}

const STATUS_COPY: Record<string, { label: string; tone: string }> = {
  charging: { label: 'PV is charging the battery', tone: 'text-emerald-600' },
  discharging: { label: 'Battery is supplying loads', tone: 'text-amber-600' },
  balanced: { label: 'Production matches demand', tone: 'text-sky-600' },
  unknown: { label: 'Awaiting telemetry', tone: 'text-slate-400' },
}

export default function InverterPowerCard({ inverterPower, systemStatus }: Props) {
  const active = inverterPower > 0.5
  const deficit = inverterPower < -0.5
  const balanced = !active && !deficit

  const valueColor = active
    ? 'text-emerald-600'
    : deficit
      ? 'text-red-500'
      : 'text-slate-400'

  const bg = active
    ? 'from-emerald-50/50'
    : deficit
      ? 'from-red-50/50'
      : 'from-slate-50/50'

  const displayValue = Math.abs(inverterPower) < 0.05 ? 0 : inverterPower
  const formatted = displayValue.toFixed(1)

  const Arrow = active ? ArrowUpIcon : deficit ? ArrowDownIcon : null
  const arrowColor = active ? 'text-emerald-500' : deficit ? 'text-red-500' : 'text-slate-300'

  const statusInfo = STATUS_COPY[systemStatus] ?? STATUS_COPY.unknown

  return (
    <div className={`bg-gradient-to-br ${bg} to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-6`}>
      <div className="flex items-start justify-between mb-3">
        <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">
          Inverter
        </span>
        {Arrow && <Arrow className={`w-5 h-5 ${arrowColor}`} />}
      </div>

      <div className="flex items-baseline gap-2">
        <AnimatePresence mode="popLayout" initial={false}>
          <motion.span
            key={formatted}
            initial={{ y: 12, opacity: 0 }}
            animate={{ y: 0, opacity: 1 }}
            exit={{ y: -12, opacity: 0 }}
            transition={{ duration: 0.25, ease: 'easeOut' }}
            className={`text-4xl font-bold tabular-nums ${valueColor}`}
          >
            {balanced ? '0' : Math.abs(displayValue).toFixed(1)}
          </motion.span>
        </AnimatePresence>
        <span className={`text-lg font-medium ${valueColor}`}>W</span>
      </div>

      <div className="mt-3 flex items-center gap-2">
        <span className={`text-sm font-medium ${valueColor}`}>
          {active && 'Supplying inverter'}
          {deficit && 'DC deficit'}
          {balanced && 'Idle'}
        </span>
      </div>

      <div className="mt-4 pt-4 border-t border-slate-100">
        <span className={`text-xs font-medium ${statusInfo.tone}`}>
          {statusInfo.label}
        </span>
      </div>
    </div>
  )
}
