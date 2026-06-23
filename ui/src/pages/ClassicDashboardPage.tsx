import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useStore, useAtomValue } from 'jotai'
import { selectedDeviceAtom, devicesAtom, devicesLoadingAtom } from '../state/atoms'
import { supabase } from '../lib/supabase'
import { loadChannels } from '../state/services/channelsService'
import { loadLayout } from '../state/services/layoutService'
import WidgetGrid from '../widgets/WidgetGrid'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'

export default function ClassicDashboardPage() {
  const navigate = useNavigate()
  const store = useStore()
  const devices = useAtomValue(devicesAtom)
  const devicesLoading = useAtomValue(devicesLoadingAtom)
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const [userId, setUserId] = useState<string | null>(null)

  useEffect(() => {
    let mounted = true
    async function loadUser() {
      const { data: { session } } = await supabase.auth.getSession()
      if (mounted && session) {
        setUserId(session.user.id)
      }
    }
    loadUser()
    return () => { mounted = false }
  }, [])

  useEffect(() => {
    if (!selectedDevice || !userId) return
    Promise.all([
      startLiveTelemetrySafe(store, selectedDevice.device_key),
      loadChannels(store, selectedDevice.device_key),
      loadLayout(store, userId),
      startAggregatesPollingSafe(store),
    ])
    return () => {
      stopLiveTelemetrySafe()
      stopAggregatesPollingSafe()
    }
  }, [selectedDevice, userId, store])

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }

  if (devicesLoading) {
    return <div className="flex items-center justify-center h-screen text-slate-500">Loading devices...</div>
  }

  const header = ({ onMenuClick }: { onMenuClick: () => void }) => (
    <HeaderBar
      devices={devices}
      selectedDeviceId={selectedDevice?.id ?? null}
      onSelectDevice={setSelectedDevice}
      isOnline={selectedDevice?.is_online ?? false}
      onMenuClick={onMenuClick}
    />
  )

  return (
    <DashboardLayout
      currentPath="/dashboard/classic"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      header={header}
      deviceName={selectedDevice?.device_name}
    >
      {!selectedDevice ? (
        <div className="text-center py-12">
          <p className="text-slate-600 mb-4">No device selected.</p>
          <p className="text-sm text-slate-400">Select a device from the dropdown above to view telemetry.</p>
        </div>
      ) : (
        <WidgetGrid />
      )}
    </DashboardLayout>
  )
}

async function startLiveTelemetrySafe(store: any, deviceKey: string) {
  const { startLiveTelemetry } = await import('../state/services/telemetryService')
  startLiveTelemetry(store, deviceKey)
}

async function stopLiveTelemetrySafe() {
  const { stopLiveTelemetry } = await import('../state/services/telemetryService')
  stopLiveTelemetry()
}

async function startAggregatesPollingSafe(store: any) {
  const { startAggregatesPolling } = await import('../state/services/aggregatesService')
  startAggregatesPolling(store)
}

async function stopAggregatesPollingSafe() {
  const { stopAggregatesPolling } = await import('../state/services/aggregatesService')
  stopAggregatesPolling()
}
