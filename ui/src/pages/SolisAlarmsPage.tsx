import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom } from 'jotai'
import { selectedDeviceAtom } from '../state/atoms'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'
import { APP_VERSION } from '../lib/version'
import SolisLayout from '../components/solis/SolisLayout'

interface Alarm {
  time: string
  message: string
  severity: 'warning' | 'critical' | 'info'
}

const severityColor: Record<string, string> = {
  warning: 'text-amber-600 bg-amber-50 border-amber-200',
  critical: 'text-red-600 bg-red-50 border-red-200',
  info: 'text-blue-600 bg-blue-50 border-blue-200',
}

export default function SolisAlarmsPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)

  useEffect(() => {
    async function load() {
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (data) setDevices(data)
    }
    load()
  }, [])

  const online = selectedDevice?.is_online ?? false

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  // Derive alarms from device state
  const alarms: Alarm[] = []
  if (!selectedDevice?.is_online) {
    alarms.push({
      time: new Date().toLocaleString(),
      message: 'Device is offline. Telemetry data may not be current.',
      severity: 'critical',
    })
  }

  return (
    <SolisLayout
      currentPath="/dashboard/alarms"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      devices={devices}
      selectedDeviceId={selectedDevice?.id ?? null}
      onSelectDevice={setSelectedDevice}
      isOnline={online}
      version={APP_VERSION}
      onRefresh={handleRefresh}
    >
      <h2 className="text-lg font-semibold text-gray-800 mb-4">Alarms</h2>

      {alarms.length === 0 ? (
        <div className="bg-white rounded-lg border border-gray-200 p-6 shadow-sm text-center">
          <div className="text-green-500 text-4xl mb-2">✓</div>
          <p className="text-gray-500">No active alarms</p>
        </div>
      ) : (
        <div className="space-y-3">
          {alarms.map((alarm, i) => (
            <div
              key={i}
              className={`rounded-lg border px-4 py-3 text-sm ${severityColor[alarm.severity]}`}
            >
              <div className="font-medium mb-1">{alarm.severity.toUpperCase()}</div>
              <div className="text-gray-700">{alarm.message}</div>
              <div className="text-xs text-gray-400 mt-1">{alarm.time}</div>
            </div>
          ))}
        </div>
      )}
    </SolisLayout>
  )
}