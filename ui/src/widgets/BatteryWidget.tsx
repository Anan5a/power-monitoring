import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { Battery0Icon } from '@heroicons/react/24/outline'
import { batteryDataAtom, batteryLoadingAtom, selectedDeviceAtom } from '../state/atoms'

function BatteryWidget() {
  const data = useAtomValue(batteryDataAtom)
  const loading = useAtomValue(batteryLoadingAtom)
  const device = useAtomValue(selectedDeviceAtom)

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
        </div>
      )}
    </div>
  )
}

export default memo(BatteryWidget)
