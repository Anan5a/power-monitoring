import { useState } from 'react'
import { Bars3Icon, ArrowPathIcon, UserCircleIcon } from '@heroicons/react/24/outline'
import type { Device } from '../../lib/types'

interface DashboardTopBarProps {
  devices: Device[]
  selectedDeviceId: string | null
  onSelectDevice: (device: Device) => void
  isOnline: boolean
  lastUpdated?: string
  version?: string
  onMenuClick: () => void
  onRefresh?: () => void
  onSignOut?: () => void
}

export default function DashboardTopBar({
  devices,
  selectedDeviceId,
  onSelectDevice,
  isOnline,
  lastUpdated,
  version,
  onMenuClick,
  onRefresh,
  onSignOut,
}: DashboardTopBarProps) {
  const selected = devices.find((d) => d.id === selectedDeviceId)
  const [menuOpen, setMenuOpen] = useState(false)

  return (
    <header className="h-14 bg-[#3c4454] text-white flex items-center justify-between px-4 shrink-0">
      <div className="flex items-center gap-4">
        <button type="button" onClick={onMenuClick} className="lg:hidden p-1 rounded hover:bg-white/10">
          <Bars3Icon className="h-6 w-6" />
        </button>
        <h1 className="text-base font-medium hidden sm:block">{selected?.device_name ?? 'Select device'}</h1>
        <span className={`h-2 w-2 rounded-full ${isOnline ? 'bg-green-400' : 'bg-gray-400'}`} />
        {lastUpdated && <span className="text-xs text-gray-300">Updated: {lastUpdated}</span>}
      </div>

      <div className="flex items-center gap-3">
        {version && (
          <span className="hidden sm:inline text-[10px] px-1.5 py-0.5 rounded bg-white/10 text-gray-300">
            v{version}
          </span>
        )}
        <select
          value={selectedDeviceId ?? ''}
          onChange={(e) => {
            const d = devices.find((x) => x.id === e.target.value)
            if (d) onSelectDevice(d)
          }}
          className="bg-white/10 text-sm rounded px-2 py-1 border border-white/20 focus:outline-none"
        >
          {devices.map((d) => (
            <option key={d.id} value={d.id} className="text-gray-900">
              {d.device_name}
            </option>
          ))}
        </select>
        <button type="button" onClick={onRefresh} className="p-1.5 rounded hover:bg-white/10">
          <ArrowPathIcon className="h-5 w-5" />
        </button>
        <div className="relative flex items-center gap-2 pl-3 border-l border-white/20">
          <button type="button" onClick={() => setMenuOpen(!menuOpen)} className="rounded hover:bg-white/10 p-0.5">
            <UserCircleIcon className="h-6 w-6 text-gray-300" />
          </button>
          {menuOpen && (
            <>
              <div className="fixed inset-0 z-10" onClick={() => setMenuOpen(false)} />
              <div className="absolute right-0 top-full mt-2 w-44 z-20 bg-white rounded-md shadow-lg border border-gray-200 py-1">
                <button
                  type="button"
                  onClick={() => { setMenuOpen(false); window.location.href = '/settings' }}
                  className="w-full text-left px-4 py-2 text-sm text-gray-700 hover:bg-gray-50"
                >
                  Settings
                </button>
                <button
                  type="button"
                  onClick={() => { setMenuOpen(false); onSignOut?.() }}
                  className="w-full text-left px-4 py-2 text-sm text-red-600 hover:bg-gray-50"
                >
                  Sign Out
                </button>
              </div>
            </>
          )}
        </div>
      </div>
    </header>
  )
}