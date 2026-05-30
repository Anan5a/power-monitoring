import type { TelemetryPoint, DeviceProfile, BatteryProfile } from '../lib/types'

interface Props {
  data: TelemetryPoint | null
  deviceProfile: DeviceProfile
  batteryProfiles: BatteryProfile[]
}

const CHEM_LABELS: Record<string, string> = {
  lead_acid: 'Pb',
  lipol: 'LiPo',
  liion: 'LiIon',
  nimh: 'NiMH',
  lifepo4: 'LiFePO4',
  agm: 'AGM',
  fla: 'FLA',
}

export default function BatteryStatus({ data, deviceProfile, batteryProfiles }: Props) {
  const socFields = deviceProfile.fields.filter(f => f.key.startsWith('soc_pct'))
  const mAhFields = deviceProfile.fields.filter(f => f.key.startsWith('coulomb_mah'))

  if (socFields.length === 0 && mAhFields.length === 0) return null

  function SocBar({ value }: { value: number }) {
    const color = value > 50 ? 'bg-green-500' : value > 20 ? 'bg-yellow-500' : 'bg-red-500'
    return (
      <div className="w-full bg-gray-200 rounded-full h-3">
        <div className={`h-3 rounded-full ${color} transition-all`} style={{ width: `${Math.min(100, Math.max(0, value))}%` }} />
      </div>
    )
  }

  // Build a map from channel index → battery profile
  const bpByChannel = new Map(batteryProfiles.map(bp => [bp.channel, bp]))

  // Group SoC fields by their channel index
  const groupByChannel = (fields: typeof socFields) => {
    return fields.map(field => {
      const ch = parseInt(field.key.match(/[0-9]+$/)?.[0] ?? 'NaN', 10)
      const bp = bpByChannel.get(ch)
      const chemLabel = bp?.chemistry ? (CHEM_LABELS[bp.chemistry] ?? bp.chemistry) : null
      const name = bp?.name || field.label
      return { field, ch, bp, chemLabel, name }
    })
  }

  const socWithInfo = groupByChannel(socFields)
  const mAhWithInfo = groupByChannel(mAhFields)

  return (
    <div className="bg-white rounded-lg shadow p-4">
      <h3 className="font-semibold text-gray-800 mb-3">Battery / SoC</h3>
      <div className="space-y-3">
        {socWithInfo.map(({ field, bp, chemLabel, name }) => {
          const val = data?.payload?.[field.key] ?? 0
          return (
            <div key={field.key}>
              <div className="flex justify-between text-sm mb-1">
                <span className="text-gray-600">
                  {name}
                  {chemLabel && <span className="ml-1 text-xs bg-gray-100 px-1 rounded">{chemLabel}</span>}
                  {bp && <span className="ml-1 text-xs text-gray-400">{(bp.capacity_mAh / 1000).toFixed(1)}Ah</span>}
                </span>
                <span className="font-medium text-gray-800">{typeof val === 'number' ? val.toFixed(1) : val}%</span>
              </div>
              <SocBar value={typeof val === 'number' ? val : 0} />
            </div>
          )
        })}
        {mAhWithInfo.map(({ field, name }) => {
          const val = data?.payload?.[field.key] ?? 0
          return (
            <div key={field.key} className="flex justify-between text-sm">
              <span className="text-gray-600">{name}</span>
              <span className="font-medium text-gray-800">{typeof val === 'number' ? val.toFixed(1) : val} mAh</span>
            </div>
          )
        })}
      </div>
    </div>
  )
}