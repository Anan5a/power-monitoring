import { useState, type ReactNode } from 'react'
import Sidebar from './Sidebar'

export interface DashboardLayoutProps {
  children: ReactNode
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  header: ReactNode | ((props: { onMenuClick: () => void }) => ReactNode)
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
  const toggleSidebar = () => setSidebarOpen(prev => !prev)

  const headerNode = typeof header === 'function' ? header({ onMenuClick: toggleSidebar }) : header

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
      <div className={`min-h-screen flex flex-col transition-all duration-300 ${sidebarOpen ? 'md:ml-64' : ''}`}>
        {headerNode}
        <main className="flex-1 p-4 md:p-6">
          {children}
        </main>
      </div>
    </div>
  )
}
