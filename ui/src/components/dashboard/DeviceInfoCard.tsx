import type { Device } from '../../lib/types'

interface DeviceInfoCardProps {
  device: Device
}

export default function DeviceInfoCard({ device }: DeviceInfoCardProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm">
      <h3 className="text-sm font-semibold text-gray-700 mb-3">Device Info</h3>
      <div className="space-y-2 text-sm">
        <div className="flex justify-between"><span className="text-gray-500">Name</span><span>{device.device_name}</span></div>
        <div className="flex justify-between"><span className="text-gray-500">Type</span><span>{device.device_type}</span></div>
        <div className="flex justify-between"><span className="text-gray-500">Key</span><span className="truncate max-w-[120px]">{device.device_key}</span></div>
        <div className="flex justify-between"><span className="text-gray-500">Online</span><span>{device.is_online ? 'Yes' : 'No'}</span></div>
      </div>
    </div>
  )
}