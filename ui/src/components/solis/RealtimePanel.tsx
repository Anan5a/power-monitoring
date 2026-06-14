import type { Device } from '../../lib/types'

interface RealtimePanelProps {
  device: Device
  currentPower: number
  dailyYield: number
  totalYield: number
  status: string
  alarmCount: number
}

export default function RealtimePanel({
  device,
  currentPower,
  dailyYield,
  totalYield,
  status,
  alarmCount,
}: RealtimePanelProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm space-y-4">
      <h3 className="text-sm font-semibold text-gray-700">Real-time Information</h3>
      <div className="grid grid-cols-2 gap-3 text-sm">
        <div><span className="text-gray-500">Status</span><div className="font-medium">{status}</div></div>
        <div><span className="text-gray-500">Current Power</span><div className="font-medium">{currentPower.toFixed(1)} W</div></div>
        <div><span className="text-gray-500">Daily Yield</span><div className="font-medium">{dailyYield.toFixed(2)} kWh</div></div>
        <div><span className="text-gray-500">Total Yield</span><div className="font-medium">{totalYield.toFixed(2)} kWh</div></div>
        <div><span className="text-gray-500">Alarms</span><div className="font-medium">{alarmCount}</div></div>
      </div>
      <h3 className="text-sm font-semibold text-gray-700 pt-2 border-t border-gray-100">Basic Information</h3>
      <div className="grid grid-cols-2 gap-3 text-sm">
        <div><span className="text-gray-500">Name</span><div className="font-medium">{device.device_name}</div></div>
        <div><span className="text-gray-500">Type</span><div className="font-medium">{device.device_type}</div></div>
      </div>
    </div>
  )
}