import { useEffect, useState } from 'react'
import { Bars3Icon } from '@heroicons/react/24/outline'
import type { Device } from '../lib/types'

export interface HeaderBarProps {
  devices: Device[]
  selectedDeviceId: string | null
  onSelectDevice: (device: Device) => void
  isOnline: boolean
  lastUpdated: Date | null
  onMenuClick: () => void
}

function formatSecondsAgo(seconds: number): string {
  if (seconds < 1) return 'just now'
  if (seconds < 60) return `${seconds} second${seconds === 1 ? '' : 's'} ago`
  const minutes = Math.floor(seconds / 60)
  return `${minutes} minute${minutes === 1 ? '' : 's'} ago`
}

export default function HeaderBar({
  devices,
  selectedDeviceId,
  onSelectDevice,
  isOnline,
  lastUpdated,
  onMenuClick,
}: HeaderBarProps) {
  const [now, setNow] = useState<Date>(() => new Date())

  useEffect(() => {
    const id = setInterval(() => setNow(new Date()), 1000)
    return () => clearInterval(id)
  }, [])

  const secondsAgo = lastUpdated
    ? Math.max(0, Math.floor((now.getTime() - lastUpdated.getTime()) / 1000))
    : null

  return (
    <header className="sticky top-0 z-20 bg-white border-b border-slate-200 px-4 py-3 flex items-center justify-between gap-4">
      <div className="flex items-center gap-3 min-w-0">
        <button
          type="button"
          aria-label="Open navigation"
          onClick={onMenuClick}
          className="inline-flex items-center justify-center w-9 h-9 rounded-lg text-slate-600 hover:bg-slate-100"
        >
          <Bars3Icon className="h-6 w-6" />
        </button>

        <select
          value={selectedDeviceId ?? ''}
          onChange={e => {
            const found = devices.find(d => d.id === e.target.value)
            if (found) onSelectDevice(found)
          }}
          className="rounded-lg border border-slate-200 bg-white text-sm px-3 py-1.5 focus:outline-none focus:ring-2 focus:ring-brand-400 max-w-[14rem] truncate"
        >
          {devices.length === 0 ? (
            <option value="">No devices</option>
          ) : (
            <>
              <option value="">-- Choose a device --</option>
              {devices.map(d => (
                <option key={d.id} value={d.id}>{d.device_name}</option>
              ))}
            </>
          )}
        </select>
      </div>

      <div className="flex items-center gap-4 text-xs text-slate-500">
        <span className="font-mono hidden sm:inline">{now.toLocaleTimeString()}</span>
        <div className="flex items-center gap-2">
          <span
            className={`inline-block w-2 h-2 rounded-full ${isOnline ? 'bg-emerald-500 animate-pulse' : 'bg-red-500'}`}
            aria-hidden
          />
          <span className="text-slate-700 font-medium">
            {isOnline ? 'Online' : 'Offline'}
          </span>
        </div>
        <span className="hidden md:inline">
          Last update:{' '}
          {secondsAgo === null ? (
            <span className="text-slate-400">--</span>
          ) : (
            <span className="text-slate-700">{formatSecondsAgo(secondsAgo)}</span>
          )}
        </span>
      </div>
    </header>
  )
}
