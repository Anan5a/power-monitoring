import { useState, type ReactNode } from 'react'
import type { Device } from '../../lib/types'
import SolisSidebar from './SolisSidebar'
import SolisTopBar from './SolisTopBar'

export interface SolisLayoutProps {
  children: ReactNode
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  devices: Device[]
  selectedDeviceId: string | null
  onSelectDevice: (device: Device) => void
  isOnline: boolean
  lastUpdated?: string
  version?: string
  onRefresh?: () => void
}

export default function SolisLayout({
  children,
  currentPath,
  onNavigate,
  onSignOut,
  devices,
  selectedDeviceId,
  onSelectDevice,
  isOnline,
  lastUpdated,
  version,
  onRefresh,
}: SolisLayoutProps) {
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false)

  return (
    <div className="min-h-screen bg-[#f0f2f5] flex flex-col">
      <SolisTopBar
        devices={devices}
        selectedDeviceId={selectedDeviceId}
        onSelectDevice={onSelectDevice}
        isOnline={isOnline}
        lastUpdated={lastUpdated}
        version={version}
        onMenuClick={() => setMobileMenuOpen(true)}
        onRefresh={onRefresh}
      />

      <div className="flex flex-1 overflow-hidden">
        {/* Desktop sidebar */}
        <div className="hidden lg:block">
          <SolisSidebar
            currentPath={currentPath}
            onNavigate={onNavigate}
            onSignOut={onSignOut}
            version={version}
          />
        </div>

        {/* Mobile drawer */}
        {mobileMenuOpen && (
          <>
            <div
              className="fixed inset-0 z-30 bg-black/50 lg:hidden"
              onClick={() => setMobileMenuOpen(false)}
            />
            <div className="fixed inset-y-0 left-0 z-40 lg:hidden">
              <SolisSidebar
                currentPath={currentPath}
                onNavigate={(path) => { onNavigate(path); setMobileMenuOpen(false) }}
                onSignOut={() => { setMobileMenuOpen(false); onSignOut() }}
                version={version}
              />
            </div>
          </>
        )}

        <main className="flex-1 overflow-y-auto p-4 md:p-6">
          <div className="max-w-[1440px] mx-auto">
            {children}
          </div>
        </main>
      </div>
    </div>
  )
}
