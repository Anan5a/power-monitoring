import { motion, AnimatePresence } from 'framer-motion'
import {
  SunIcon,
  BoltIcon,
  Cog6ToothIcon,
  ShieldCheckIcon,
  ArrowLeftOnRectangleIcon,
} from '@heroicons/react/24/outline'

export interface SidebarProps {
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  isOpen: boolean
  onClose: () => void
  deviceName?: string
}

interface NavItem {
  label: string
  path: string
  Icon: typeof SunIcon
}

const NAV_ITEMS: NavItem[] = [
  { label: 'Dashboard', path: '/dashboard', Icon: SunIcon },
  { label: 'Dashboard (Legacy)', path: '/dashboard/legacy', Icon: SunIcon },
  { label: 'Channels', path: '/channels', Icon: BoltIcon },
  { label: 'Settings', path: '/settings', Icon: Cog6ToothIcon },
  { label: 'Admin', path: '/admin', Icon: ShieldCheckIcon },
]

export default function Sidebar({
  currentPath,
  onNavigate,
  onSignOut,
  isOpen,
  onClose,
  deviceName = 'IoT Dashboard',
}: SidebarProps) {
  function handleNavigate(path: string) {
    onNavigate(path)
    onClose()
  }

  const navContent = (
    <>
      <div className="px-6 py-5 border-b border-slate-700/50">
        <div className="text-lg font-semibold text-white tracking-tight">
          {deviceName}
        </div>
        <div className="text-xs text-slate-400 mt-0.5">Power Monitor</div>
      </div>

      <nav className="flex-1 px-3 py-4 space-y-1 overflow-y-auto">
        {NAV_ITEMS.map(({ label, path, Icon }) => {
          const isActive = currentPath === path
          return (
            <button
              key={path}
              type="button"
              onClick={() => handleNavigate(path)}
              aria-current={isActive ? 'page' : undefined}
              className={
                isActive
                  ? 'w-full flex items-center gap-3 px-4 py-2.5 text-sm font-medium rounded-lg bg-slate-700 text-white'
                  : 'w-full flex items-center gap-3 px-4 py-2.5 text-sm font-medium rounded-lg text-slate-300 hover:bg-slate-700/50 hover:text-white transition-colors'
              }
            >
              <Icon className="h-5 w-5 shrink-0" />
              <span>{label}</span>
            </button>
          )
        })}
      </nav>

      <div className="px-3 py-4 border-t border-slate-700/50">
        <button
          type="button"
          onClick={onSignOut}
          className="w-full flex items-center gap-3 px-4 py-2.5 text-sm font-medium rounded-lg text-slate-400 hover:bg-slate-700/50 hover:text-white transition-colors"
        >
          <ArrowLeftOnRectangleIcon className="h-5 w-5 shrink-0" />
          <span>Sign Out</span>
        </button>
      </div>
    </>
  )

  return (
    <>
      {/* Mobile overlay drawer */}
      <AnimatePresence>
        {isOpen && (
          <>
            <motion.div
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              exit={{ opacity: 0 }}
              transition={{ duration: 0.2 }}
              className="fixed inset-0 z-30 bg-black/50 md:hidden"
              onClick={onClose}
            />
            <motion.aside
              key="mobile-drawer"
              initial={{ x: '-100%' }}
              animate={{ x: 0 }}
              exit={{ x: '-100%' }}
              transition={{ type: 'tween', duration: 0.25, ease: 'easeInOut' }}
              className="fixed inset-y-0 left-0 z-40 w-64 bg-gradient-to-b from-slate-900 to-slate-800 text-slate-100 flex flex-col md:hidden"
            >
              {navContent}
            </motion.aside>
          </>
        )}
      </AnimatePresence>

      {/* Desktop sidebar — collapsible */}
      <aside
        className={`hidden md:flex fixed inset-y-0 left-0 z-40 bg-gradient-to-b from-slate-900 to-slate-800 text-slate-100 flex-col transition-all duration-300 overflow-hidden ${
          isOpen ? 'w-64' : 'w-0'
        }`}
      >
        {navContent}
      </aside>
    </>
  )
}
