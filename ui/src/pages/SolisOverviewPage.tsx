import { useEffect, useMemo, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useAtomValue } from 'jotai'
import { selectedDeviceAtom, connectionStateAtom } from '../state/atoms'
import { computedTelemetryAtom, liveBufferAtom, secondsAgoAtom } from '../state/derived'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'
import { APP_VERSION } from '../lib/version'
import SolisLayout from '../components/solis/SolisLayout'
import SemiGauge from '../components/solis/SemiGauge'
import KpiCard from '../components/solis/KpiCard'
import StatusBadge from '../components/solis/StatusBadge'
import DateRangeTabs from '../components/solis/DateRangeTabs'
import SolisChart from '../components/solis/SolisChart'
import AlarmPanel from '../components/solis/AlarmPanel'
import DeviceInfoCard from '../components/solis/DeviceInfoCard'
import TopologyDiagram from '../components/solis/TopologyDiagram'

type Tab = 'overview' | 'yield' | 'flow'
type Range = 'day' | 'month' | 'year' | 'total'

export default function SolisOverviewPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const telemetry = useAtomValue(computedTelemetryAtom)
  const buffer = useAtomValue(liveBufferAtom)
  const connection = useAtomValue(connectionStateAtom)
  const secondsAgo = useAtomValue(secondsAgoAtom)

  useEffect(() => {
    async function load() {
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (data) setDevices(data)
    }
    load()
  }, [])

  const [activeTab, setActiveTab] = useState<Tab>('overview')
  const [range, setRange] = useState<Range>('day')

  const online = selectedDevice?.is_online ?? false
  const status = connection === 'live' ? 'normal' : 'offline'
  const alarmCount = 0

  const kpiSub = useMemo(() => {
    const cap = 'Installed Capacity: 4.2 kWp'
    const totalYield = `Total Yield: ${(telemetry.total_energy_wh / 1000).toFixed(1)} kWh`
    return { cap, totalYield }
  }, [telemetry])

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  if (!selectedDevice) {
    return (
      <SolisLayout
        currentPath="/dashboard"
        onNavigate={handleNavigate}
        onSignOut={handleSignOut}
        devices={devices}
        selectedDeviceId={null}
        onSelectDevice={setSelectedDevice}
        isOnline={false}
        version={APP_VERSION}
        lastUpdated={secondsAgo != null ? `${secondsAgo}s ago` : undefined}
        onRefresh={handleRefresh}
      >
        <div className="text-center py-20 text-gray-500">Select a device to view telemetry.</div>
      </SolisLayout>
    )
  }

  return (
    <SolisLayout
      currentPath="/dashboard"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      devices={devices}
      selectedDeviceId={selectedDevice.id}
      onSelectDevice={setSelectedDevice}
      isOnline={online}
      version={APP_VERSION}
      onRefresh={handleRefresh}
    >
      {/* KPI row */}
      <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-4 gap-4 mb-6">
        <KpiCard
          label="Current Power"
          value={telemetry.pv_power.toFixed(1)}
          unit="W"
          subText={kpiSub.cap}
          icon={<SemiGauge value={telemetry.pv_power} max={4200} size={56} stroke={8} />}
          accent="bg-orange-500"
        />
        <KpiCard
          label="Daily Yield"
          value={(telemetry.total_energy_wh / 1000).toFixed(1)}
          unit="kWh"
          subText={kpiSub.totalYield}
          icon={<SemiGauge value={telemetry.total_energy_wh / 1000} max={42} size={56} stroke={8} />}
          accent="bg-orange-500"
        />
        <KpiCard
          label="Battery SoC"
          value={telemetry.min_soc_pct?.toFixed(0) ?? '--'}
          unit="%"
          subText={`Status: ${telemetry.system_status}`}
          icon={<SemiGauge value={telemetry.min_soc_pct ?? 0} max={100} size={56} stroke={8} color="#22c55e" />}
          accent="bg-green-500"
        />
        <KpiCard
          label="System Status"
          value={status}
          subText={<StatusBadge status={status} label={status === 'normal' ? 'Normal' : 'Offline'} />}
          icon={<div className="h-7 w-7 rounded-full bg-white/20" />}
          accent="bg-[#3c4454]"
        />
      </div>

      {/* Tabs */}
      <div className="flex items-center justify-between mb-4">
        <div className="inline-flex rounded-md border border-gray-300 overflow-hidden bg-white">
          {[
            { key: 'overview', label: 'Overview' },
            { key: 'yield', label: 'Yield Chart' },
            { key: 'flow', label: 'Power Flow' },
          ].map((t) => (
            <button
              key={t.key}
              type="button"
              onClick={() => setActiveTab(t.key as Tab)}
              className={`px-4 py-1.5 text-sm font-medium border-r border-gray-200 last:border-r-0 ${
                activeTab === t.key ? 'bg-[#3c4454] text-white' : 'text-gray-600 hover:bg-gray-50'
              }`}
            >
              {t.label}
            </button>
          ))}
        </div>
        <DateRangeTabs value={range} onChange={setRange} />
      </div>

      {/* Tab content */}
      {activeTab === 'overview' && (
        <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
          <div className="lg:col-span-2">
            <SolisChart data={buffer} metric="pv_power" />
          </div>
          <div className="space-y-4">
            <AlarmPanel alarmCount={alarmCount} />
            <DeviceInfoCard device={selectedDevice} />
          </div>
        </div>
      )}

      {activeTab === 'yield' && (
        <SolisChart data={buffer} metric="pv_power" />
      )}

      {activeTab === 'flow' && (
        <TopologyDiagram
          pvPower={telemetry.pv_power / 1000}
          gridPower={0}
          batteryPower={telemetry.battery_power / 1000}
          loadPower={telemetry.dc_load_power / 1000}
          batterySoc={telemetry.min_soc_pct}
        />
      )}
    </SolisLayout>
  )
}