import { motion, AnimatePresence } from 'framer-motion'
import { Battery0Icon } from '@heroicons/react/24/outline'
import { useBatteryCharge } from '../hooks/useBatteryCharge'

interface Props {
  deviceId: string  // devices.id (uuid), not device_key
}

export default function BatteryChargeCard({ deviceId }: Props) {
  const {
    chargeWh, capacityWh,
    energyIn24h, energyOut24h,
    isFullChargeToday, isLoading
  } = useBatteryCharge(deviceId)

  const displayPct = capacityWh > 0 ? (chargeWh / capacityWh) * 100 : 0
  const barColor = displayPct > 50 ? 'bg-emerald-500' : displayPct > 20 ? 'bg-amber-400' : 'bg-red-500'

  return (
    <div className="bg-gradient-to-br from-emerald-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
      {/* Header */}
      <div className="flex items-start justify-between mb-3">
        <div className="flex items-center gap-2">
          <Battery0Icon className="w-5 h-5 text-emerald-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Battery</span>
        </div>
        <span className="text-xs text-emerald-600 font-medium">Wh</span>
      </div>

      <AnimatePresence mode="popLayout" initial={false}>
        <motion.div
          key={chargeWh.toFixed(0)}
          initial={{ y: 8, opacity: 0 }}
          animate={{ y: 0, opacity: 1 }}
          exit={{ y: -8, opacity: 0 }}
          transition={{ duration: 0.3 }}
          className="flex items-baseline gap-1.5 mb-1"
        >
          {isLoading ? (
            <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
          ) : (
            <>
              <span className="text-4xl font-bold text-emerald-600 tabular-nums">
                {chargeWh > 0 ? chargeWh.toFixed(0) : '0'}
              </span>
              <span className="text-lg font-medium text-emerald-500">
                / {capacityWh.toFixed(0)} Wh
              </span>
            </>
          )}
        </motion.div>
      </AnimatePresence>

      {/* Progress bar */}
      {capacityWh > 0 && (
        <div className="mt-3">
          <div className="w-full bg-slate-100 rounded-full h-2.5 overflow-hidden">
            <div
              className={`h-2.5 rounded-full ${barColor} transition-all duration-500`}
              style={{ width: `${Math.min(displayPct, 100)}%` }}
            />
          </div>
          <div className="flex items-center justify-between mt-1.5">
            <span className="text-[10px] text-slate-400">
              {displayPct.toFixed(1)}%
              {isFullChargeToday && <span className="ml-1 text-emerald-500">● full</span>}
            </span>
            <span className="text-[10px] text-emerald-500">
              +{energyIn24h.toFixed(1)} / -{energyOut24h.toFixed(1)} Wh (24h)
            </span>
          </div>
        </div>
      )}

      {!isLoading && (
        <div className="mt-3 pt-3 border-t border-slate-100">
          <span className="text-[10px] text-slate-400">
            Trigger computes charge_wh from battery_power integration · voltage anchor at {29.2}V
          </span>
        </div>
      )}
    </div>
  )
}