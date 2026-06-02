interface Props {
  vcName: string
  voltage: number | null
  current: number | null
  power: number | null
  energyWh: number | null
  socPct: number | null
  batteryCapacity: number
  online: boolean
}

export default function VCDashboardCard({
  vcName, voltage, current, power, energyWh,
  socPct, batteryCapacity, online
}: Props) {
  const hasBattery = batteryCapacity > 0
  // HOTFIX: add offset until coulomb counter can be physically reset
  const displayEnergyWh = hasBattery && energyWh !== null ? energyWh + 1300 : energyWh
  const socWarning = hasBattery && socPct !== null && socPct < 20
  const borderColor = socWarning ? 'border-l-yellow-400' : 'border-l-emerald-500'

  return (
    <div
      className={`bg-white rounded-2xl shadow-[0_1px_2px_rgba(15,23,42,0.04),0_1px_3px_rgba(15,23,42,0.06)] hover:shadow-[0_4px_12px_rgba(0,0,0,0.08)] transition-shadow duration-200 border border-slate-100 border-l-4 ${borderColor} p-4`}
    >
      {/* Header */}
      <div className="flex items-center justify-between mb-4">
        <div className="flex items-center gap-2">
          <span className="font-semibold text-slate-800">{vcName}</span>
          <span className={`w-2 h-2 rounded-full ${online ? 'bg-emerald-400' : 'bg-slate-300'}`} />
        </div>
      </div>

      {/* V / I / P row */}
      <div className="grid grid-cols-3 gap-2 mb-4">
        <div className="text-center">
          <div className="text-xs text-slate-400">V</div>
          <div className="text-2xl font-bold text-slate-800">
            {voltage !== null ? voltage.toFixed(2) : '--'}
          </div>
        </div>
        <div className="text-center">
          <div className="text-xs text-slate-400">A</div>
          <div className="text-2xl font-bold text-slate-800">
            {current !== null ? current.toFixed(2) : '--'}
          </div>
        </div>
        <div className="text-center">
          <div className="text-xs text-slate-400">W</div>
          <div className="text-2xl font-bold text-slate-800">
            {power !== null ? power.toFixed(1) : '--'}
          </div>
        </div>
      </div>

      {/* Energy */}
      {displayEnergyWh !== null && (
        <div className="text-center mb-4">
          <span className="text-xs text-slate-400">Energy: </span>
          <span className="text-sm font-medium text-slate-700">
            {displayEnergyWh >= 1000 ? `${(displayEnergyWh / 1000).toFixed(2)} kWh` : `${displayEnergyWh.toFixed(1)} Wh`}
          </span>
        </div>
      )}

      {/* SoC bar (only if battery configured) */}
      {hasBattery && (
        <div>
          <div className="flex justify-between text-xs text-slate-500 mb-1">
            <span>SoC</span>
            <span>{socPct !== null ? `${socPct.toFixed(0)}%` : '--'}</span>
          </div>
          <div className="w-full bg-slate-100 rounded-full h-2 overflow-hidden">
            {socPct !== null && (
              <div
                className="h-2 rounded-full bg-gradient-to-r from-emerald-400 to-teal-500 transition-all duration-300"
                style={{ width: `${Math.min(socPct, 100)}%` }}
              />
            )}
          </div>
        </div>
      )}
    </div>
  )
}
