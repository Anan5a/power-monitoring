import {
  HomeIcon,
  BoltIcon,
  ChartBarIcon,
  BellIcon,
  Cog6ToothIcon,
  ArrowRightStartOnRectangleIcon,
  Squares2X2Icon,
} from '@heroicons/react/24/outline'

export interface DashboardSidebarProps {
  currentPath: string
  onNavigate: (path: string) => void
  onSignOut: () => void
  version?: string
  isCollapsed?: boolean
}

interface NavItem {
  label: string
  path: string
  Icon: typeof HomeIcon
}

const MAIN_ITEMS: NavItem[] = [
  { label: 'Overview', path: '/dashboard', Icon: HomeIcon },
  { label: 'Device', path: '/dashboard/device', Icon: BoltIcon },
  { label: 'Analysis', path: '/dashboard/analysis', Icon: ChartBarIcon },
  { label: 'Alarms', path: '/dashboard/alarms', Icon: BellIcon },
]

const BOTTOM_ITEMS: NavItem[] = [
  { label: 'Settings', path: '/settings', Icon: Cog6ToothIcon },
  { label: 'Classic View', path: '/dashboard/classic', Icon: Squares2X2Icon },
]

export default function DashboardSidebar({ currentPath, onNavigate, onSignOut, version, isCollapsed }: DashboardSidebarProps) {
  function Item({ label, path, Icon }: NavItem) {
    const active = currentPath === path || (path !== '/dashboard' && currentPath.startsWith(path))
    return (
      <button
        type="button"
        onClick={() => onNavigate(path)}
        className={`w-full flex items-center gap-3 px-4 py-3 text-sm font-medium transition-colors ${
          isCollapsed ? 'justify-center px-0' : 'px-4'
        } ${
          active
            ? 'text-accent bg-white/10 border-l-4 border-accent'
            : 'text-gray-300 hover:text-white hover:bg-white/5 border-l-4 border-transparent'
        }`}
      >
        <Icon className="h-5 w-5 shrink-0" />
        {!isCollapsed && <span className="truncate">{label}</span>}
      </button>
    )
  }

  return (
    <aside className={`h-full bg-sidebar text-gray-100 flex flex-col shrink-0 transition-all duration-300 ${isCollapsed ? 'w-16' : 'w-sidebar'}`}>
      <div className={`h-14 flex items-center border-b border-white/10 ${isCollapsed ? 'justify-center px-0' : 'px-4'}`}>
        <BoltIcon className="h-6 w-6 text-accent" />
        {!isCollapsed && <span className="ml-2 font-semibold tracking-tight">PowerMonitor</span>}
      </div>

      <nav className="flex-1 py-2 space-y-1">
        {MAIN_ITEMS.map((item) => <Item key={item.path} {...item} />)}
      </nav>

      <div className="py-2 border-t border-white/10">
        {BOTTOM_ITEMS.map((item) => <Item key={item.path} {...item} />)}
        <button
          type="button"
          onClick={onSignOut}
          className={`w-full flex items-center gap-3 py-3 text-sm font-medium text-gray-400 hover:text-white hover:bg-white/5 border-l-4 border-transparent ${
            isCollapsed ? 'justify-center px-0' : 'px-4'
          }`}
        >
          <ArrowRightStartOnRectangleIcon className="h-5 w-5 shrink-0" />
          {!isCollapsed && <span>Sign Out</span>}
        </button>
      </div>

      {version && (
        <div className={`py-2 text-[10px] text-gray-500 border-t border-white/10 text-center ${!isCollapsed && 'px-4 text-left'}`}>
          v{version}
        </div>
      )}
    </aside>
  )
}
