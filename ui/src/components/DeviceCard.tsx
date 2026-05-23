import type { Device } from '../lib/types'

interface Props {
  device: Device
}

export default function DeviceCard({ device }: Props) {
  function timeAgo(date: string) {
    const seconds = Math.floor((Date.now() - new Date(date).getTime()) / 1000)
    if (seconds < 60) return `${seconds}s ago`
    if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`
    return `${Math.floor(seconds / 86400)}d ago`
  }

  return (
    <div className="bg-white rounded-lg shadow p-4">
      <div className="flex items-center gap-3">
        <div className={`w-3 h-3 rounded-full ${device.is_online ? 'bg-green-500' : 'bg-gray-400'}`} />
        <div>
          <h3 className="font-semibold text-gray-800">{device.device_name}</h3>
          <p className="text-sm text-gray-500">
            {device.is_online ? 'Online' : `Last seen ${timeAgo(device.last_seen_at)}`}
          </p>
        </div>
        <span className="ml-auto px-2 py-1 bg-gray-100 text-gray-600 text-xs rounded">
          {device.device_type}
        </span>
      </div>
    </div>
  )
}