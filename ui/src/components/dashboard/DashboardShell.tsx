import { useState, type ReactNode } from 'react'
import type { Device } from '../../lib/types'
import DashboardSidebar from './DashboardSidebar'
import DashboardTopBar from './DashboardTopBar'

export interface DashboardShellProps {
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

export default function DashboardShell({
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
}: DashboardShellProps) {
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false)
  const [isCollapsed, setIsCollapsed] = useState(false)

  return (
    <div className="min-h-screen bg-page flex flex-col">
      <DashboardTopBar
        devices={devices}
        selectedDeviceId={selectedDeviceId}
        onSelectDevice={onSelectDevice}
        isOnline={isOnline}
        lastUpdated={lastUpdated}
        version={version}
        onMenuClick={() => setMobileMenuOpen(true)}
        onCollapseToggle={() => setIsCollapsed(!isCollapsed)}
        onRefresh={onRefresh}
        onSignOut={onSignOut}
      />

      <div className="flex flex-1 overflow-hidden">
        {/* Desktop sidebar */}
        <div className={`hidden lg:block transition-all duration-300 ${isCollapsed ? 'w-16' : 'w-sidebar'}`}>
          <DashboardSidebar
            currentPath={currentPath}
            onNavigate={onNavigate}
            onSignOut={onSignOut}
            version={version}
            isCollapsed={isCollapsed}
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
              <DashboardSidebar
                currentPath={currentPath}
                onNavigate={(path) => { onNavigate(path); setMobileMenuOpen(false) }}
                onSignOut={() => { setMobileMenuOpen(false); onSignOut() }}
                version={version}
                isCollapsed={false}
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
