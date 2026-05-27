import { useEffect, useState, useCallback } from 'react'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { useRealtime } from '../hooks/useRealtime'
import type { Device, DeviceProfile, TelemetryPoint, DeviceChannels } from '../lib/types'
import DeviceCard from '../components/DeviceCard'
import TelemetryChart from '../components/TelemetryChart'
import ChannelSelector from '../components/ChannelSelector'
import RelayControl from '../components/RelayControl'
import BatteryStatus from '../components/BatteryStatus'

// Unit inference from key suffix
function inferUnit(key: string): string {
  if (key.endsWith('_V')) return 'V'
  if (key.endsWith('_I')) return 'A'
  if (key.endsWith('_P')) return 'W'
  if (key.endsWith('_mah') || key.endsWith('_mah')) return 'mAh'
  if (key.endsWith('_soc') || key.endsWith('_pct')) return '%'
  return ''
}

// Human-readable label from key
function keyToLabel(key: string): string {
  return key
    .replace(/^ina3221_v/, 'INA3221 V Ch')
    .replace(/^ina3221_i/, 'INA3221 I Ch')
    .replace(/^ina226_/, 'INA226 ')
    .replace(/^ads1115_/, 'ADS1115 ')
    .replace(/^coulomb_mah/, 'Coulomb mAh Ch')
    .replace(/^soc_pct/, 'SoC Ch')
    .replace(/^ch(\d+)_V/, 'CH$1 Voltage')
    .replace(/^ch(\d+)_I/, 'CH$1 Current')
    .replace(/^ch(\d+)_P/, 'CH$1 Power')
    .replace(/_/g, ' ')
}

interface FieldDef { key: string; label: string; unit: string; chart: boolean }

export default function DashboardPage() {
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [deviceProfile, setDeviceProfile] = useState<DeviceProfile | null>(null)
  const [deviceChannels, setDeviceChannels] = useState<DeviceChannels | null>(null)
  const [historicalData, setHistoricalData] = useState<TelemetryPoint[]>([])
  const [selectedFields, setSelectedFields] = useState<string[]>([])
  const [loadingDevices, setLoadingDevices] = useState(true)
  const [dynamicFields, setDynamicFields] = useState<FieldDef[]>([])
  const [fieldsSource, setFieldsSource] = useState<'profile' | 'dynamic'>('profile')

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

  const discoverFieldsFromTelemetry = useCallback(async (deviceKey: string) => {
    // Fetch latest telemetry row to discover available payload keys
    const { data } = await supabase
      .from('telemetry_live')
      .select('payload')
      .eq('device_id', deviceKey)
      .order('recorded_at', { ascending: false })
      .limit(1)
      .maybeSingle()

    if (!data?.payload) return

    const payload = data.payload as Record<string, unknown>
    const discoveredKeys = Object.keys(payload).filter(k => typeof payload[k] === 'number')

    const fields: FieldDef[] = discoveredKeys.map(key => ({
      key,
      label: keyToLabel(key),
      unit: inferUnit(key),
      chart: true,
    }))

    setDynamicFields(fields)
    setFieldsSource('dynamic')
    setSelectedFields(fields.filter(f => f.chart).map(f => f.key))
  }, [supabase])

  const loadDeviceProfile = useCallback(async (device: Device) => {
    const { data } = await supabase
      .from('device_profiles')
      .select('*')
      .eq('device_type', device.device_type)
      .maybeSingle()

    if (data) {
      setDeviceProfile(data)
      const defaults = data.fields.filter((f: { key: string; chart: boolean }) => f.chart).map((f: { key: string }) => f.key)
      setSelectedFields(defaults)
    }
  }, [supabase])

  const loadDeviceChannels = useCallback(async (deviceKey: string) => {
    const channels = await fetchDeviceChannels(deviceKey)
    setDeviceChannels(channels)
  }, [])

  const loadHistory = useCallback(async (device: Device) => {
    const { data } = await supabase
      .from('telemetry_live')
      .select('*')
      .eq('device_id', device.device_key)
      .order('recorded_at', { ascending: true })
      .limit(200)

    if (data) setHistoricalData(data)
  }, [supabase])

  useEffect(() => {
    if (!selectedDevice) return

    loadDeviceChannels(selectedDevice.device_key)
    loadHistory(selectedDevice)
    // Try dynamic discovery first; fall back to deviceProfile if no telemetry yet
    discoverFieldsFromTelemetry(selectedDevice.device_key).then(() => {
      // If no fields discovered (no telemetry rows yet), fall back to profile
      setFieldsSource(prev => prev === 'profile' ? prev : 'dynamic')
      loadDeviceProfile(selectedDevice)
    })
  }, [selectedDevice])

  const { dataPoints, latestReading } = useRealtime(selectedDevice?.device_key ?? null)

  // Use dynamic fields when available, otherwise fall back to deviceProfile.fields
  const activeFields: FieldDef[] = fieldsSource === 'dynamic' && dynamicFields.length > 0
    ? dynamicFields
    : (deviceProfile?.fields ?? []) as FieldDef[]

  // Build merged field list: apply channel_name overrides
  const mergedFields = (() => {
    const overrides = new Map(deviceChannels?.channel_names.map(cn => [cn.channel, cn.name]) ?? [])
    return activeFields.map(f => {
      const ch = parseInt(f.key.match(/[0-9]+$/)?.[0] ?? 'NaN', 10)
      if (!isNaN(ch) && overrides.has(ch)) {
        return { ...f, label: overrides.get(ch)! }
      }
      return f
    })
  })()

  const chartFields = mergedFields.filter(f => selectedFields.includes(f.key))

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
            <a href="/settings" className="text-blue-600 hover:underline text-sm">Settings</a>
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
                setDynamicFields([])
                setFieldsSource('profile')
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

        {selectedDevice && activeFields.length > 0 && (
          <>
            <div className="grid grid-cols-1 md:grid-cols-3 gap-6 mb-6">
              <DeviceCard device={selectedDevice} />
              {selectedDevice.device_type === 'power-monitor' && (
                <RelayControl deviceKey={selectedDevice.device_key} />
              )}
              <BatteryStatus
                data={latestReading}
                deviceProfile={deviceProfile ?? { id: 0, device_type: '', label: '', fields: activeFields }}
                batteryProfiles={deviceChannels?.battery_profiles ?? []}
              />
            </div>

            <div className="bg-white rounded-lg shadow p-4 mb-6">
              <div className="flex items-center justify-between mb-4">
                <h2 className="text-lg font-semibold text-gray-800">Live Charts</h2>
                <ChannelSelector
                  fields={mergedFields}
                  groups={deviceChannels?.channel_groups ?? []}
                  channelNames={deviceChannels?.channel_names ?? []}
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
                        {field.label} {field.unit ? `(${field.unit})` : ''}
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