interface Props {
  vcName: string
  voltage: number | null
  current: number | null
  power: number | null
  socPct: number | null
  batteryCapacity: number
  relayOn: boolean
  online: boolean
}

export default function VCDashboardCard({
  vcName, voltage, current, power,
  socPct, batteryCapacity, relayOn, online
}: Props) {
  const hasBattery = batteryCapacity > 0

  // Border color: green=normal, yellow=warning (SoC<20%), red=danger (relay on)
  const socWarning = hasBattery && socPct !== null && socPct < 20
  const borderColor = relayOn ? 'border-red-500' : socWarning ? 'border-yellow-400' : 'border-green-500'
  const socBarColor = !hasBattery ? '' : socPct === null ? '' : socPct > 50 ? 'bg-green-500' : socPct > 20 ? 'bg-yellow-400' : 'bg-red-500'

  return (
    <div className={`bg-white rounded-lg shadow border-l-4 ${borderColor} p-4`}>
      {/* Header */}
      <div className="flex items-center justify-between mb-3">
        <div className="flex items-center gap-2">
          <span className="font-semibold text-gray-800">{vcName}</span>
          <span className={`w-2 h-2 rounded-full ${online ? 'bg-green-400' : 'bg-gray-300'}`} />
        </div>
        <span className={`text-xs px-2 py-0.5 rounded font-medium ${relayOn ? 'bg-red-100 text-red-700' : 'bg-gray-100 text-gray-500'}`}>
          {relayOn ? 'RELAY ON' : 'RELAY OFF'}
        </span>
      </div>

      {/* V / I / P row */}
      <div className="grid grid-cols-3 gap-2 mb-3">
        <div className="text-center">
          <div className="text-xs text-gray-400">V</div>
          <div className="text-lg font-medium text-gray-700">
            {voltage !== null ? voltage.toFixed(2) : '--'}
          </div>
        </div>
        <div className="text-center">
          <div className="text-xs text-gray-400">I</div>
          <div className="text-lg font-medium text-gray-700">
            {current !== null ? current.toFixed(2) : '--'}
          </div>
        </div>
        <div className="text-center">
          <div className="text-xs text-gray-400">W</div>
          <div className="text-lg font-medium text-blue-600">
            {power !== null ? power.toFixed(1) : '--'}
          </div>
        </div>
      </div>

      {/* SoC bar (only if battery configured) */}
      {hasBattery && (
        <div>
          <div className="flex justify-between text-xs text-gray-500 mb-1">
            <span>SoC</span>
            <span>{socPct !== null ? `${socPct.toFixed(0)}%` : '--'}</span>
          </div>
          <div className="w-full bg-gray-200 rounded-full h-2">
            {socPct !== null && (
              <div
                className={`h-2 rounded-full ${socBarColor}`}
                style={{ width: `${Math.min(socPct, 100)}%` }}
              />
            )}
          </div>
        </div>
      )}
    </div>
  )
}