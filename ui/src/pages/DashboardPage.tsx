import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'
import { useRealtime } from '../hooks/useRealtime'
import type { Device, DeviceProfile, TelemetryPoint } from '../lib/types'
import DeviceCard from '../components/DeviceCard'
import TelemetryChart from '../components/TelemetryChart'
import ChannelSelector from '../components/ChannelSelector'
import RelayControl from '../components/RelayControl'
import BatteryStatus from '../components/BatteryStatus'

export default function DashboardPage() {
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [deviceProfile, setDeviceProfile] = useState<DeviceProfile | null>(null)
  const [historicalData, setHistoricalData] = useState<TelemetryPoint[]>([])
  const [selectedFields, setSelectedFields] = useState<string[]>([])
  const [loadingDevices, setLoadingDevices] = useState(true)

  useEffect(() => {
    async function loadDevices() {
      const { data: { session } } = await supabase.auth.getSession()
      if (!session) return

      const { data, error } = await supabase
        .from('devices')
        .select('*')
        .order('device_name')

      if (!error && data) setDevices(data)
      setLoadingDevices(false)
    }
    loadDevices()
  }, [])

  useEffect(() => {
    if (!selectedDevice) return

    async function loadDeviceProfile() {
      const { data } = await supabase
        .from('device_profiles')
        .select('*')
        .eq('device_type', selectedDevice.device_type)
        .single()

      if (data) {
        setDeviceProfile(data)
        const defaults = data.fields.filter((f: { key: string; chart: boolean }) => f.chart).map((f: { key: string }) => f.key)
        setSelectedFields(defaults)
      }
    }

    async function loadHistory() {
      const { data } = await supabase
        .from('telemetry_live')
        .select('*')
        .eq('device_id', selectedDevice.device_key)
        .order('recorded_at', { ascending: true })
        .limit(200)

      if (data) setHistoricalData(data)
    }

    loadDeviceProfile()
    loadHistory()
  }, [selectedDevice])

  const { dataPoints, latestReading } = useRealtime(selectedDevice?.device_key ?? null)

  const chartFields = deviceProfile?.fields.filter(f => selectedFields.includes(f.key)) ?? []

  if (loadingDevices) {
    return <div className="flex items-center justify-center h-screen text-gray-500">Loading...</div>
  }

  return (
    <div className="min-h-screen bg-gray-100">
      <header className="bg-white shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <h1 className="text-xl font-bold text-gray-800">IoT Dashboard</h1>
          <div className="flex gap-4">
            <a href="/admin" className="text-blue-600 hover:underline text-sm">Admin</a>
            <button
              onClick={() => supabase.auth.signOut()}
              className="text-gray-600 hover:text-gray-800 text-sm"
            >
              Sign Out
            </button>
          </div>
        </div>
      </header>

      <main className="max-w-7xl mx-auto px-4 py-6">
        {devices.length === 0 ? (
          <div className="text-center py-12">
            <p className="text-gray-600 mb-4">No devices registered yet.</p>
            <a href="/admin" className="bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700">
              Add a Device
            </a>
          </div>
        ) : (
          <div className="mb-6">
            <label className="block text-sm font-medium text-gray-700 mb-2">Select Device</label>
            <select
              value={selectedDevice?.id ?? ''}
              onChange={e => {
                const d = devices.find(dev => dev.id === e.target.value)
                setSelectedDevice(d ?? null)
                setHistoricalData([])
              }}
              className="w-full max-w-xs rounded-md border border-gray-300 px-3 py-2 bg-white"
            >
              <option value="">-- Choose a device --</option>
              {devices.map(d => (
                <option key={d.id} value={d.id}>{d.device_name} ({d.device_type})</option>
              ))}
            </select>
          </div>
        )}

        {selectedDevice && deviceProfile && (
          <>
            <div className="grid grid-cols-1 md:grid-cols-3 gap-6 mb-6">
              <DeviceCard device={selectedDevice} />
              {selectedDevice.device_type === 'power-monitor' && (
                <RelayControl deviceKey={selectedDevice.device_key} />
              )}
              <BatteryStatus data={latestReading} deviceProfile={deviceProfile} />
            </div>

            <div className="bg-white rounded-lg shadow p-4 mb-6">
              <div className="flex items-center justify-between mb-4">
                <h2 className="text-lg font-semibold text-gray-800">Live Charts</h2>
                <ChannelSelector
                  fields={deviceProfile.fields}
                  selected={selectedFields}
                  onChange={setSelectedFields}
                />
              </div>
              {chartFields.length === 0 ? (
                <p className="text-gray-500 text-sm">Select fields above to display charts.</p>
              ) : (
                <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
                  {chartFields.map(field => (
                    <div key={field.key}>
                      <h3 className="text-sm font-medium text-gray-600 mb-2">
                        {field.label} ({field.unit})
                      </h3>
                      <TelemetryChart
                        data={[...historicalData, ...dataPoints]}
                        dataKey={field.key}
                        color="blue"
                      />
                    </div>
                  ))}
                </div>
              )}
            </div>
          </>
        )}
      </main>
    </div>
  )
}