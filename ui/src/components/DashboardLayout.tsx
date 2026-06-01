import { useState, type ReactNode } from 'react'
import Sidebar from './Sidebar'

export interface DashboardLayoutProps {
  children: ReactNode
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  header: ReactNode
  deviceName?: string
}

export default function DashboardLayout({
  children,
  currentPath,
  onNavigate,
  onSignOut,
  header,
  deviceName,
}: DashboardLayoutProps) {
  const [sidebarOpen, setSidebarOpen] = useState(false)

  return (
    <div className="min-h-screen bg-slate-50">
      <Sidebar
        currentPath={currentPath}
        onNavigate={(path) => { onNavigate(path); setSidebarOpen(false) }}
        onSignOut={onSignOut}
        isOpen={sidebarOpen}
        onClose={() => setSidebarOpen(false)}
        deviceName={deviceName}
      />
      <div className="md:ml-64 min-h-screen flex flex-col">
        {header}
        <main className="flex-1 p-4 md:p-6">
          {children}
        </main>
      </div>
    </div>
  )
}