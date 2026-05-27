import type { DeviceChannels } from '../lib/types'

interface Props {
  latestReading: Record<string, number> | null
  deviceChannels: DeviceChannels | null
  relayOn: boolean[]
}

export default function QuickStatsRow({ latestReading, deviceChannels, relayOn }: Props) {
  // Sum all power values from payload
  const totalPower = latestReading
    ? Object.entries(latestReading)
        .filter(([k]) => k.endsWith('_P') || k.endsWith('_p'))
        .reduce((sum, [, v]) => sum + (v as number), 0)
    : 0

  const batteryProfiles = deviceChannels?.battery_profiles ?? []
  return (
    <div className="flex flex-wrap gap-4 items-center justify-between bg-white rounded-lg shadow px-6 py-4 mb-6">
      {/* Total power */}
      <div className="flex items-center gap-2">
        <span className="text-sm text-gray-500">Total Power</span>
        <span className="text-xl font-bold text-blue-600">
          {totalPower > 0 ? `${totalPower.toFixed(1)} W` : '--'}
        </span>
      </div>

      {/* SoC bars for VCs with batteries */}
      <div className="flex gap-4 items-center">
        {batteryProfiles.map((bp, idx) => {
          if (!bp.capacity_mAh || bp.capacity_mAh === 0) return null
          const socKey = `soc_pct${idx}`
          const soc = latestReading?.[socKey] ?? null
          const color = soc === null ? 'bg-gray-300' : soc > 50 ? 'bg-green-500' : soc > 20 ? 'bg-yellow-400' : 'bg-red-500'
          return (
            <div key={idx} className="flex items-center gap-2">
              <span className="text-xs text-gray-500">VC{idx}</span>
              <div className="w-20 bg-gray-200 rounded-full h-2">
                {soc !== null && (
                  <div className={`h-2 rounded-full ${color}`} style={{ width: `${Math.min(soc, 100)}%` }} />
                )}
              </div>
              <span className="text-xs font-medium text-gray-700">
                {soc !== null ? `${soc.toFixed(0)}%` : '--'}
              </span>
            </div>
          )
        })}
      </div>

      {/* Relay chips */}
      <div className="flex gap-2">
        {[0, 1, 2, 3].map(i => (
          <span
            key={i}
            className={`text-xs px-2 py-1 rounded font-medium ${
              relayOn[i] ? 'bg-red-100 text-red-700' : 'bg-gray-100 text-gray-500'
            }`}
          >
            R{i}: {relayOn[i] ? 'ON' : 'OFF'}
          </span>
        ))}
      </div>
    </div>
  )
}