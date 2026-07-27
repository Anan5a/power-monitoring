import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useStore, useAtomValue } from 'jotai'
import { selectedDeviceAtom, devicesAtom, devicesLoadingAtom } from '../state/atoms'
import { pvPowerAtom, batteryPowerAtom, inverterPowerAtom } from '../state/derived'
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
  const pvPower = useAtomValue(pvPowerAtom)
  const batteryPower = useAtomValue(batteryPowerAtom)
  const inverterPower = useAtomValue(inverterPowerAtom)

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

  // Update <title> with PV / battery / load / inverter status
  useEffect(() => {
    const batIcon = batteryPower > 5 ? '↑' : batteryPower < -5 ? '↓' : '—'
    const invIcon = inverterPower > 5 ? '→' : inverterPower < -5 ? '←' : '—'
    const title = `☀${pvPower.toFixed(0)} 🔋${batIcon}${Math.abs(batteryPower).toFixed(0)} ${invIcon}${Math.abs(inverterPower).toFixed(0)}`
    document.title = title
  }, [pvPower, batteryPower, inverterPower])

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
