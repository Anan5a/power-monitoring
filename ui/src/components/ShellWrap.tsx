import type { ReactNode } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useAtomValue } from 'jotai'
import { selectedDeviceAtom, devicesAtom } from '../state/atoms'
import { secondsAgoAtom } from '../state/derived'
import { supabase } from '../lib/supabase'
import { APP_VERSION } from '../lib/version'
import DashboardShell from './dashboard/DashboardShell'

interface ShellWrapProps {
  children: ReactNode
  currentPath: string
}

export default function ShellWrap({ children, currentPath }: ShellWrapProps) {
  const navigate = useNavigate()
  const devices = useAtomValue(devicesAtom)
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const secondsAgo = useAtomValue(secondsAgoAtom)

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  return (
    <DashboardShell
      currentPath={currentPath}
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      devices={devices}
      selectedDeviceId={selectedDevice?.id ?? null}
      onSelectDevice={setSelectedDevice}
      isOnline={selectedDevice?.is_online ?? false}
      lastUpdated={secondsAgo != null ? `${secondsAgo}s ago` : undefined}
      version={APP_VERSION}
      onRefresh={handleRefresh}
    >
      {children}
    </DashboardShell>
  )
}