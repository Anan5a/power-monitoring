import type { TelemetryPoint, DeviceProfile } from '../lib/types'

interface Props {
  data: TelemetryPoint | null
  deviceProfile: DeviceProfile
}

export default function BatteryStatus({ data, deviceProfile }: Props) {
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

  return (
    <div className="bg-white rounded-lg shadow p-4">
      <h3 className="font-semibold text-gray-800 mb-3">Battery / SoC</h3>
      <div className="space-y-3">
        {socFields.map((field, i) => {
          const val = data?.payload?.[field.key] ?? 0
          return (
            <div key={field.key}>
              <div className="flex justify-between text-sm mb-1">
                <span className="text-gray-600">{field.label}</span>
                <span className="font-medium text-gray-800">{typeof val === 'number' ? val.toFixed(1) : val}%</span>
              </div>
              <SocBar value={typeof val === 'number' ? val : 0} />
            </div>
          )
        })}
        {mAhFields.map(field => {
          const val = data?.payload?.[field.key] ?? 0
          return (
            <div key={field.key} className="flex justify-between text-sm">
              <span className="text-gray-600">{field.label}</span>
              <span className="font-medium text-gray-800">{typeof val === 'number' ? val.toFixed(1) : val} mAh</span>
            </div>
          )
        })}
      </div>
    </div>
  )
}