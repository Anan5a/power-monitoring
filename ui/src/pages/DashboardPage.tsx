import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useStore } from 'jotai'
import { selectedDeviceAtom } from '../state/atoms'
import { supabase } from '../lib/supabase'
import { loadChannels } from '../state/services/channelsService'
import { loadLayout } from '../state/services/layoutService'
import type { Device } from '../lib/types'
import WidgetGrid from '../widgets/WidgetGrid'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'

export default function DashboardPage() {
  const navigate = useNavigate()
  const store = useStore()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const [userId, setUserId] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let mounted = true
    async function load() {
      const { data: { session } } = await supabase.auth.getSession()
      if (!session || !mounted) return
      setUserId(session.user.id)
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (mounted) {
        if (data) setDevices(data)
        setLoading(false)
      }
    }
    load()
    return () => { mounted = false }
  }, [])

  useEffect(() => {
    if (!selectedDevice || !userId) return
    Promise.all([
      startLiveTelemetrySafe(store, selectedDevice.device_key),
      loadChannels(store, selectedDevice.device_key),
      loadLayout(store, userId),
    ])
    return () => {
      stopLiveTelemetrySafe()
    }
  }, [selectedDevice, userId, store])

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }

  if (loading) {
    return <div className="flex items-center justify-center h-screen text-slate-500">Loading...</div>
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
      currentPath="/dashboard"
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