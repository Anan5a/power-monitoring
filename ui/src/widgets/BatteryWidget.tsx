import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { Battery0Icon } from '@heroicons/react/24/outline'
import { batteryDataAtom, batteryLoadingAtom, selectedDeviceAtom } from '../state/atoms'
import { batteryPowerAtom } from '../state/derived'

const IDLE_DEADBAND_W = 5

function formatDuration(hours: number): string {
  if (hours >= 99) return '>99h'
  const totalMinutes = Math.round(hours * 60)
  const h = Math.floor(totalMinutes / 60)
  const m = totalMinutes % 60
  if (h === 0) return `${m}m`
  return `${h}h ${m}m`
}

function BatteryWidget() {
  const data = useAtomValue(batteryDataAtom)
  const loading = useAtomValue(batteryLoadingAtom)
  const device = useAtomValue(selectedDeviceAtom)
  const batteryPower = useAtomValue(batteryPowerAtom)

  if (!device) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 p-4 flex items-center text-slate-300 text-sm">
        Select a device
      </div>
    )
  }

  const chargeWh = data?.chargeWh ?? 0
  const capacityWh = data?.capacityWh ?? 0
  const displayPct = capacityWh > 0 ? (chargeWh / capacityWh) * 100 : 0
  const barColor = displayPct > 50 ? 'bg-emerald-500' : displayPct > 20 ? 'bg-amber-400' : 'bg-red-500'

  let etaLabel: string | null = null
  if (capacityWh > 0) {
    if (batteryPower > IDLE_DEADBAND_W) {
      const remainingWh = capacityWh - chargeWh
      if (remainingWh > 0) etaLabel = `${formatDuration(remainingWh / batteryPower)} to full`
    } else if (batteryPower < -IDLE_DEADBAND_W) {
      if (chargeWh > 0) etaLabel = `${formatDuration(chargeWh / -batteryPower)} remaining`
    }
  }

  return (
    <div className="h-full w-full bg-gradient-to-br from-emerald-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-4">
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <Battery0Icon className="w-5 h-5 text-emerald-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Battery</span>
        </div>
        <span className="text-xs text-emerald-600 font-medium">Wh</span>
      </div>
      <div className="flex items-baseline gap-1.5 mb-1 min-h-[36px]">
        {loading ? (
          <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
        ) : (
          <>
            <span className="text-2xl font-bold text-emerald-600 tabular-nums">{chargeWh > 0 ? chargeWh.toFixed(0) : '0'}</span>
            <span className="text-sm font-medium text-emerald-500">/ {capacityWh.toFixed(0)} Wh</span>
          </>
        )}
      </div>
      {capacityWh > 0 && (
        <div className="mt-2">
          <div className="w-full bg-slate-100 rounded-full h-2 overflow-hidden">
            <div className={`h-2 rounded-full ${barColor} transition-all duration-500`} style={{ width: `${Math.min(displayPct, 100)}%` }} />
          </div>
          <div className="flex items-center justify-between mt-1">
            <span className="text-[10px] text-slate-400">
              {displayPct.toFixed(1)}%{data?.isFullChargeToday && <span className="ml-1 text-emerald-500">● full</span>}
            </span>
            <span className="text-[10px] text-emerald-500">
              +{(data?.energyIn24h ?? 0).toFixed(1)} / -{(data?.energyOut24h ?? 0).toFixed(1)} Wh (24h)
            </span>
          </div>
          {etaLabel && (
            <div className="mt-1 text-sm text-slate-400 text-right">{etaLabel}</div>
          )}
        </div>
      )}
    </div>
  )
}

export default memo(BatteryWidget)
