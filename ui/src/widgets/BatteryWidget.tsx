import { memo, useEffect, useState } from 'react'
import { Battery0Icon } from '@heroicons/react/24/outline'
import { supabase } from '../lib/supabase'

interface Props {
  deviceId: string
}

interface BatteryState {
  chargeWh: number
  capacityWh: number
  energyIn24h: number
  energyOut24h: number
  isFullChargeToday: boolean
}

function BatteryWidget({ deviceId }: Props) {
  const [state, setState] = useState<BatteryState>({ chargeWh: 0, capacityWh: 0, energyIn24h: 0, energyOut24h: 0, isFullChargeToday: false })
  const [isLoading, setIsLoading] = useState(true)

  useEffect(() => {
    if (!deviceId) return
    let cancelled = false
    const fetch = async () => {
      const { data, error } = await supabase.rpc('get_battery_charge', { p_device_id: deviceId, p_hours: 24 })
      if (cancelled) return
      setIsLoading(false)
      if (error || !data) return
      const row = Array.isArray(data) ? data[0] : data
      setState({
        chargeWh: row.charge_wh ?? 0,
        capacityWh: row.capacity_wh ?? 0,
        energyIn24h: row.energy_in_24h ?? 0,
        energyOut24h: row.energy_out_24h ?? 0,
        isFullChargeToday: row.is_full_charge_today ?? false,
      })
    }
    fetch()
    const id = setInterval(fetch, 10000)
    return () => { cancelled = true; clearInterval(id) }
  }, [deviceId])

  const displayPct = state.capacityWh > 0 ? (state.chargeWh / state.capacityWh) * 100 : 0
  const barColor = displayPct > 50 ? 'bg-emerald-500' : displayPct > 20 ? 'bg-amber-400' : 'bg-red-500'

  return (
    <div className="h-full w-full bg-gradient-to-br from-emerald-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-5">
      <div className="flex items-start justify-between mb-2">
        <div className="flex items-center gap-2">
          <Battery0Icon className="w-5 h-5 text-emerald-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Battery</span>
        </div>
        <span className="text-xs text-emerald-600 font-medium">Wh</span>
      </div>
      <div className="flex items-baseline gap-1.5 mb-1 min-h-[36px]">
        {isLoading ? (
          <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
        ) : (
          <>
            <span className="text-3xl font-bold text-emerald-600 tabular-nums">{state.chargeWh > 0 ? state.chargeWh.toFixed(0) : '0'}</span>
            <span className="text-base font-medium text-emerald-500">/ {state.capacityWh.toFixed(0)} Wh</span>
          </>
        )}
      </div>
      {state.capacityWh > 0 && (
        <div className="mt-2">
          <div className="w-full bg-slate-100 rounded-full h-2 overflow-hidden">
            <div className={`h-2 rounded-full ${barColor} transition-all duration-500`} style={{ width: `${Math.min(displayPct, 100)}%` }} />
          </div>
          <div className="flex items-center justify-between mt-1">
            <span className="text-[10px] text-slate-400">
              {displayPct.toFixed(1)}%{state.isFullChargeToday && <span className="ml-1 text-emerald-500">● full</span>}
            </span>
            <span className="text-[10px] text-emerald-500">
              +{state.energyIn24h.toFixed(1)} / -{state.energyOut24h.toFixed(1)} Wh (24h)
            </span>
          </div>
        </div>
      )}
    </div>
  )
}

export default memo(BatteryWidget)
