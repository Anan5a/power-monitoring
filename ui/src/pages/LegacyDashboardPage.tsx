import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { useRealtime } from '../hooks/useRealtime'
import type { Device, DeviceChannels } from '../lib/types'
import VCDashboardCard from '../components/VCDashboardCard'
import PowerHistoryChart from '../components/PowerHistoryChart'
import QuickStatsRow from '../components/QuickStatsRow'
import RelaySwitchRow from '../components/RelaySwitchRow'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'
import InverterPowerCard from '../components/InverterPowerCard'
import DailyGenerationCard from '../components/DailyGenerationCard'
import BatteryChargeCard from '../components/BatteryChargeCard'
import { computeTelemetry, type ComputedValues } from '../lib/computedTelemetry'

export default function DashboardPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [deviceChannels, setDeviceChannels] = useState<DeviceChannels | null>(null)
  const [loadingDevices, setLoadingDevices] = useState(true)

  useEffect(() => {
    async function load() {
      const { data: { session } } = await supabase.auth.getSession()
      if (!session) return
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (data) setDevices(data)
      setLoadingDevices(false)
    }
    load()
  }, [])

  useEffect(() => {
    if (!selectedDevice) { setDeviceChannels(null); return }
    fetchDeviceChannels(selectedDevice.device_key).then(setDeviceChannels)
  }, [selectedDevice])

  const { latestReading, isStale } = useRealtime(selectedDevice?.device_key ?? null)

  const payload = latestReading?.payload as Record<string, number> ?? null
  const computed: ComputedValues = payload
    ? computeTelemetry(payload, deviceChannels?.channel_groups ?? [], deviceChannels?.battery_profiles ?? [])
    : { pv_power: 0, battery_power: 0, battery_charging_power: 0, battery_discharging_power: 0, dc_load_power: 0, unclassified_power: 0, inverter_power: 0, system_status: 'unknown', min_soc_pct: null, max_soc_pct: null, total_energy_wh: 0 }

  const vcCards = [0, 1, 2, 3].map(vcIdx => {
    const vcName = deviceChannels?.channel_names?.find(cn => cn.channel === vcIdx)?.name ?? `VC${vcIdx}`
    const batteryCapacity = deviceChannels?.battery_profiles?.[vcIdx]?.capacity_mAh ?? 0
    return {
      vcIdx,
      vcName,
      voltage: payload?.[`ch${vcIdx}_V`] ?? null,
      current: payload?.[`ch${vcIdx}_I`] ?? null,
      power: payload?.[`ch${vcIdx}_P`] ?? null,
      energyWh: payload?.[`energy_wh${vcIdx}`] ?? null,
      socPct: payload?.[`soc_pct${vcIdx}`] ?? null,
      batteryCapacity,
      online: selectedDevice?.is_online ?? false,
    }
  })

  function handleNavigate(path: string) {
    navigate(path)
  }

  function handleSignOut() {
    supabase.auth.signOut()
    navigate('/login')
  }

  if (loadingDevices) {
    return <div className="flex items-center justify-center h-screen text-slate-500">Loading...</div>
  }

  const header = ({ onMenuClick }: { onMenuClick: () => void }) => (
    <HeaderBar
      devices={devices}
      selectedDeviceId={selectedDevice?.id ?? null}
      onSelectDevice={setSelectedDevice}
      isOnline={selectedDevice?.is_online ?? false}
      lastUpdated={latestReading ? new Date(latestReading.recorded_at) : null}
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
        <div className="space-y-6">
          {/* Top metrics row: DailyGenerationCard + InverterPowerCard */}
          <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
            <DailyGenerationCard deviceKey={selectedDevice.device_key} />
            <InverterPowerCard
              inverterPower={computed.inverter_power}
              systemStatus={computed.system_status}
            />
            <BatteryChargeCard deviceId={selectedDevice.id} />
          </div>

          {/* Relay switches */}
          <RelaySwitchRow deviceKey={selectedDevice.device_key} />

          {/* Quick stats row */}
          <QuickStatsRow
            latestReading={payload}
            deviceChannels={deviceChannels}
            relayOn={[false, false, false, false]}
            isStale={isStale}
          />

          {/* VC cards */}
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
            {vcCards.map(card => (
              <VCDashboardCard
                key={card.vcIdx}
                vcName={card.vcName}
                voltage={card.voltage}
                current={card.current}
                power={card.power}
                energyWh={card.energyWh}
                socPct={card.socPct}
                batteryCapacity={card.batteryCapacity}
                online={card.online}
              />
            ))}
          </div>

          {/* Power history chart */}
          <PowerHistoryChart deviceKey={selectedDevice.device_key} deviceChannels={deviceChannels} />
        </div>
      )}
    </DashboardLayout>
  )
}