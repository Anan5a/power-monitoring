import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useAtomValue } from 'jotai'
import { selectedDeviceAtom, connectionStateAtom } from '../state/atoms'
import { computedTelemetryAtom, channelPayloadAtomFamily, liveBufferAtom } from '../state/derived'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'
import { APP_VERSION } from '../lib/version'
import SolisLayout from '../components/solis/SolisLayout'
import TopologyDiagram from '../components/solis/TopologyDiagram'
import RealtimePanel from '../components/solis/RealtimePanel'
import ParamTable from '../components/solis/ParamTable'

export default function SolisDevicePage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const telemetry = useAtomValue(computedTelemetryAtom)
  const buffer = useAtomValue(liveBufferAtom)
  const connection = useAtomValue(connectionStateAtom)
  const ch0 = useAtomValue(channelPayloadAtomFamily(0))
  const ch1 = useAtomValue(channelPayloadAtomFamily(1))
  const ch2 = useAtomValue(channelPayloadAtomFamily(2))
  const ch3 = useAtomValue(channelPayloadAtomFamily(3))

  useEffect(() => {
    async function load() {
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (data) setDevices(data)
    }
    load()
  }, [])

  const online = selectedDevice?.is_online ?? false
  const status = connection === 'live' ? 'Online' : 'Offline'

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  const pvRows = [
    { string: 'PV1', power_kw: ch0.power != null ? (ch0.power / 1000).toFixed(3) : '--', daily_kwh: ch0.energyWh != null ? (ch0.energyWh / 1000).toFixed(3) : '--', voltage: ch0.voltage != null ? ch0.voltage.toFixed(1) : '--', current: ch0.current != null ? ch0.current.toFixed(2) : '--' },
    { string: 'PV2', power_kw: ch1.power != null ? (ch1.power / 1000).toFixed(3) : '--', daily_kwh: ch1.energyWh != null ? (ch1.energyWh / 1000).toFixed(3) : '--', voltage: ch1.voltage != null ? ch1.voltage.toFixed(1) : '--', current: ch1.current != null ? ch1.current.toFixed(2) : '--' },
    { string: 'PV3', power_kw: ch2.power != null ? (ch2.power / 1000).toFixed(3) : '--', daily_kwh: ch2.energyWh != null ? (ch2.energyWh / 1000).toFixed(3) : '--', voltage: ch2.voltage != null ? ch2.voltage.toFixed(1) : '--', current: ch2.current != null ? ch2.current.toFixed(2) : '--' },
    { string: 'PV4', power_kw: ch3.power != null ? (ch3.power / 1000).toFixed(3) : '--', daily_kwh: ch3.energyWh != null ? (ch3.energyWh / 1000).toFixed(3) : '--', voltage: ch3.voltage != null ? ch3.voltage.toFixed(1) : '--', current: ch3.current != null ? ch3.current.toFixed(2) : '--' },
  ]

  const acRows = buffer.length > 0 ? [{
    phase: 'L1',
    voltage: '--',
    current: '--',
    frequency: '--',
  }] : []

  if (!selectedDevice) {
    return (
      <SolisLayout
        currentPath="/dashboard/device"
        onNavigate={handleNavigate}
        onSignOut={handleSignOut}
        devices={devices}
        selectedDeviceId={null}
        onSelectDevice={setSelectedDevice}
        isOnline={false}
        version={APP_VERSION}
        onRefresh={handleRefresh}
      >
        <div className="text-center py-20 text-gray-500">Select a device to view details.</div>
      </SolisLayout>
    )
  }

  return (
    <SolisLayout
      currentPath="/dashboard/device"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      devices={devices}
      selectedDeviceId={selectedDevice.id}
      onSelectDevice={setSelectedDevice}
      isOnline={online}
      version={APP_VERSION}
      onRefresh={handleRefresh}
    >
      <div className="space-y-6">
        <div>
          <h2 className="text-lg font-semibold text-gray-800">Device Overview</h2>
          <p className="text-sm text-gray-500">{selectedDevice.device_key} &middot; Last updated: --</p>
        </div>

        {/* Topology */}
        <TopologyDiagram
          pvPower={telemetry.pv_power / 1000}
          gridPower={0}
          batteryPower={telemetry.battery_power / 1000}
          loadPower={telemetry.dc_load_power / 1000}
          batterySoc={telemetry.min_soc_pct}
        />

        {/* Side-by-side: Realtime panel + Basic info */}
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          <RealtimePanel
            device={selectedDevice}
            currentPower={telemetry.pv_power}
            dailyYield={telemetry.total_energy_wh / 1000}
            totalYield={telemetry.total_energy_wh / 1000}
            status={status}
            alarmCount={0}
          />
          <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm">
            <h3 className="text-sm font-semibold text-gray-700 mb-3">Basic Information</h3>
            <div className="space-y-2 text-sm">
              <div className="flex justify-between"><span className="text-gray-500">Name</span><span>{selectedDevice.device_name}</span></div>
              <div className="flex justify-between"><span className="text-gray-500">Serial</span><span>{selectedDevice.device_key}</span></div>
              <div className="flex justify-between"><span className="text-gray-500">Model</span><span>{selectedDevice.device_type}</span></div>
              <div className="flex justify-between"><span className="text-gray-500">Online</span><span>{selectedDevice.is_online ? 'Yes' : 'No'}</span></div>
            </div>
          </div>
        </div>

        {/* Parameter tables */}
        <ParamTable
          title="PV Section"
          columns={[
            { key: 'string', label: 'String' },
            { key: 'power_kw', label: 'Power (kW)' },
            { key: 'daily_kwh', label: 'Daily Yield (kWh)' },
            { key: 'voltage', label: 'U (V)' },
            { key: 'current', label: 'I (A)' },
          ]}
          rows={pvRows}
        />

        {acRows.length > 0 && (
          <ParamTable
            title="AC Parameters"
            columns={[
              { key: 'phase', label: 'Phase' },
              { key: 'voltage', label: 'U (V)' },
              { key: 'current', label: 'I (A)' },
              { key: 'frequency', label: 'F (Hz)' },
            ]}
            rows={acRows}
          />
        )}
      </div>
    </SolisLayout>
  )
}