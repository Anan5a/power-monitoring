import { motion, AnimatePresence } from 'framer-motion'
import { SunIcon, Battery0Icon, ArrowUpIcon, ArrowDownIcon } from '@heroicons/react/24/outline'
import type { DeviceChannels } from '../lib/types'
import { computeTelemetry } from '../lib/computedTelemetry'

interface Props {
  latestReading: Record<string, number> | null
  deviceChannels: DeviceChannels | null
  relayOn: boolean[]
  isStale?: boolean
}

const STATUS_STYLES: Record<string, { dot: string; text: string; label: string }> = {
  charging: { dot: 'bg-emerald-500', text: 'text-emerald-700', label: 'Charging' },
  discharging: { dot: 'bg-amber-500', text: 'text-amber-700', label: 'Discharging' },
  balanced: { dot: 'bg-sky-500', text: 'text-sky-700', label: 'Balanced' },
  unknown: { dot: 'bg-slate-400', text: 'text-slate-600', label: 'Unknown' },
}

function AnimatedNumber({ value, suffix = '', decimals = 1 }: { value: number; suffix?: string; decimals?: number }) {
  const display = Math.abs(value) < 0.05 && value !== 0 ? '0' : value.toFixed(decimals)
  return (
    <AnimatePresence mode="popLayout" initial={false}>
      <motion.span
        key={value.toFixed(decimals)}
        initial={{ y: 8, opacity: 0 }}
        animate={{ y: 0, opacity: 1 }}
        exit={{ y: -8, opacity: 0 }}
        transition={{ duration: 0.2 }}
        className="inline-block"
      >
        {display}
        {suffix}
      </motion.span>
    </AnimatePresence>
  )
}

function MetricChip({
  label,
  value,
  unit,
  color,
  icon,
}: {
  label: string
  value: number | null
  unit: string
  color: string
  icon?: React.ReactNode
}) {
  return (
    <div className="flex items-center gap-2.5">
      {icon && <div className={`shrink-0 ${color}`}>{icon}</div>}
      <div className="flex flex-col leading-tight">
        <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">{label}</span>
        <span className={`text-lg font-semibold tabular-nums ${color}`}>
          {value === null || value === undefined ? (
            <span className="text-slate-300">--</span>
          ) : (
            <>
              <AnimatedNumber value={value} />
              <span className="text-sm font-normal text-slate-400 ml-0.5">{unit}</span>
            </>
          )}
        </span>
      </div>
    </div>
  )
}

function DirectionalMetric({
  label,
  value,
  unit,
}: {
  label: string
  value: number
  unit: string
}) {
  const positive = value > 0.5
  const negative = value < -0.5
  const color = positive ? 'text-emerald-600' : negative ? 'text-cyan-600' : 'text-slate-400'
  const Icon = positive ? ArrowUpIcon : negative ? ArrowDownIcon : null

  return (
    <div className="flex items-center gap-2.5">
      <div className="shrink-0">
        {Icon ? <Icon className={`w-5 h-5 ${color}`} /> : <div className="w-5 h-5" />}
      </div>
      <div className="flex flex-col leading-tight">
        <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">{label}</span>
        <span className={`text-lg font-semibold tabular-nums ${color}`}>
          <AnimatedNumber value={Math.abs(value)} />
          <span className="text-sm font-normal text-slate-400 ml-0.5">{unit}</span>
        </span>
      </div>
    </div>
  )
}

export default function QuickStatsRow({ latestReading, deviceChannels, isStale }: Props) {
  const payload = latestReading ?? {}
  const groups = deviceChannels?.channel_groups ?? []
  const batteryProfiles = deviceChannels?.battery_profiles ?? []
  const computed = computeTelemetry(payload, groups, batteryProfiles)

  // Total load demand: everything the system is powering right now
  const totalPower = Math.abs(computed.inverter_power) + computed.dc_load_power

  const status = STATUS_STYLES[computed.system_status]

  return (
    <div className={`bg-white rounded-2xl shadow-sm border border-slate-100 px-6 py-4 mb-6 h-20 overflow-hidden ${isStale ? 'opacity-60' : ''}`}>
      <div className="flex flex-nowrap items-center justify-between gap-x-6 gap-y-4 overflow-x-auto pb-1 -mx-1 px-1">
        <div className="flex items-center gap-4">
          <div className="flex flex-col leading-tight">
            <span className="text-[11px] uppercase tracking-wide text-slate-400 font-medium">Total Power</span>
            <span className="text-2xl font-bold text-slate-800 tabular-nums">
              {totalPower > 0 ? (
                <>
                  <AnimatedNumber value={totalPower} />
                  <span className="text-base font-normal text-slate-400 ml-1">W</span>
                </>
              ) : (
                <span className="text-slate-300">--</span>
              )}
            </span>
          </div>

          <div className="h-10 w-px bg-slate-200" />

          <MetricChip
            label="PV"
            value={computed.pv_power}
            unit="W"
            color="text-amber-500"
            icon={<SunIcon className="w-5 h-5" />}
          />

          <div className="h-10 w-px bg-slate-200" />

          <DirectionalMetric
            label="Inverter"
            value={computed.inverter_power}
            unit="W"
          />

          <div className="h-10 w-px bg-slate-200" />

          <DirectionalMetric
            label="Battery"
            value={computed.battery_power}
            unit="W"
          />
        </div>

        <div className="flex flex-wrap items-center gap-4">
          {batteryProfiles.map((bp, idx) => {
            if (!bp.capacity_mAh || bp.capacity_mAh === 0) return null
            const socKey = `soc_pct${idx}`
            const soc = payload[socKey] ?? null
            const colorBar = soc === null ? 'bg-slate-200' : soc > 50 ? 'bg-emerald-500' : soc > 20 ? 'bg-amber-400' : 'bg-red-500'
            return (
              <div key={idx} className="flex items-center gap-2">
                <Battery0Icon className="w-4 h-4 text-slate-400" />
                <span className="text-[11px] text-slate-500 font-medium uppercase tracking-wide">
                  {bp.name || `VC${idx}`}
                </span>
                <div className="w-20 bg-slate-100 rounded-full h-1.5 overflow-hidden">
                  {soc !== null && (
                    <div className={`h-1.5 rounded-full ${colorBar} transition-all duration-300`} style={{ width: `${Math.min(soc, 100)}%` }} />
                  )}
                </div>
                <span className="text-xs font-semibold text-slate-700 tabular-nums w-9 text-right">
                  {soc !== null ? `${soc.toFixed(0)}%` : '--'}
                </span>
              </div>
            )
          })}

          {isStale && (
            <div className="mt-3 pt-3 border-t border-slate-100">
              <span className="text-[10px] text-slate-400 italic">Data may be stale — realtime disconnected</span>
            </div>
          )}

          <div className="h-8 w-px bg-slate-200" />

          <div className="flex items-center gap-2 px-2.5 py-1 rounded-full bg-slate-50">
            <span className={`w-2 h-2 rounded-full ${status.dot} ${computed.system_status !== 'unknown' && computed.system_status !== 'balanced' ? 'animate-pulse' : ''}`} />
            <span className={`text-xs font-semibold ${status.text}`}>{status.label}</span>
          </div>
        </div>
      </div>
    </div>
  )
}
