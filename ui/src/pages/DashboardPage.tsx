import { useEffect, useState } from 'react'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { useRealtime } from '../hooks/useRealtime'
import type { Device, DeviceChannels } from '../lib/types'
import VCDashboardCard from '../components/VCDashboardCard'
import PowerHistoryChart from '../components/PowerHistoryChart'
import QuickStatsRow from '../components/QuickStatsRow'
import RelayControl from '../components/RelayControl'

// Live clock + last-updated display
function StatusBar({ lastUpdated }: { lastUpdated: Date | null }) {
  const [now, setNow] = useState(new Date())
  useEffect(() => {
    const id = setInterval(() => setNow(new Date()), 1000)
    return () => clearInterval(id)
  }, [])
  return (
    <div className="bg-gray-800 text-gray-300 text-xs px-4 py-1.5 flex justify-between">
      <span>Local: {now.toLocaleTimeString()}</span>
      {lastUpdated && (
        <span>Last update: {lastUpdated.toLocaleTimeString()}{lastUpdated.getSeconds() === now.getSeconds() ? ' (just now)' : ''}</span>
      )}
    </div>
  )
}

// Find voltage/current/power key for a given VC from the payload keys
export default function DashboardPage() {
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [deviceChannels, setDeviceChannels] = useState<DeviceChannels | null>(null)
  const [relayOn, setRelayOn] = useState<boolean[]>([false, false, false, false])
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

  // Load device channels and relay states when device changes
  useEffect(() => {
    if (!selectedDevice) { setDeviceChannels(null); return }
    fetchDeviceChannels(selectedDevice.device_key).then(setDeviceChannels)
    supabase
      .from('relay_states')
      .select('*')
      .eq('device_key', selectedDevice.device_key)
      .then(({ data }) => {
        if (data) {
          const states = [false, false, false, false]
          data.forEach((r: { relay_index: number; is_energized: boolean }) => {
            if (r.relay_index >= 0 && r.relay_index < 4) states[r.relay_index] = r.is_energized
          })
          setRelayOn(states)
        }
      })
  }, [selectedDevice])

  const { latestReading } = useRealtime(selectedDevice?.device_key ?? null)

  // Build VC data for cards
  const vcCards = [0, 1, 2, 3].map(vcIdx => {
    if (!latestReading) {
      return { vcIdx, vcName: `VC${vcIdx}`, voltage: null, current: null, power: null, socPct: null, batteryCapacity: 0, relayOn: false }
    }
    const payload = latestReading.payload as Record<string, number>
    const payloadKeys = Object.keys(payload)

    // Find keys for this VC
    const vKey = payloadKeys.find(k => k.match(new RegExp(`_v${vcIdx}$`))) ?? null
    const iKey = payloadKeys.find(k => k.match(new RegExp(`_i${vcIdx}$`))) ?? null
    const pKey = payloadKeys.find(k => k.match(new RegExp(`_p${vcIdx}$`))) ?? null
    const socKey = payloadKeys.find(k => k.match(new RegExp(`soc_pct${vcIdx}$`))) ?? null

    const vcName = deviceChannels?.channel_names?.find(cn => cn.channel === vcIdx)?.name ?? `VC${vcIdx}`
    const batteryCapacity = deviceChannels?.battery_profiles?.[vcIdx]?.capacity_mAh ?? 0

    return {
      vcIdx,
      vcName,
      voltage: vKey !== null ? (payload[vKey] as number) : null,
      current: iKey !== null ? (payload[iKey] as number) : null,
      power: pKey !== null ? (payload[pKey] as number) : null,
      socPct: socKey !== null ? (payload[socKey] as number) : null,
      batteryCapacity,
      relayOn: relayOn[vcIdx],
    }
  })

  if (loadingDevices) {
    return <div className="flex items-center justify-center h-screen text-gray-500">Loading...</div>
  }

  return (
    <div className="min-h-screen bg-gray-100">
      <header className="bg-white shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <h1 className="text-xl font-bold text-gray-800">IoT Dashboard</h1>
          <nav className="flex gap-6 text-sm">
            <a href="/dashboard" className="text-blue-600 font-medium hover:underline">Dashboard</a>
            <a href="/channels" className="text-blue-600 hover:underline">Channels</a>
            <a href="/settings" className="text-blue-600 hover:underline">Settings</a>
            <a href="/admin" className="text-blue-600 hover:underline">Admin</a>
            <button onClick={() => supabase.auth.signOut()} className="text-gray-500 hover:text-gray-700">Sign Out</button>
          </nav>
        </div>
      </header>

      <StatusBar lastUpdated={latestReading ? new Date(latestReading.recorded_at) : null} />

      <main className="max-w-7xl mx-auto px-4 py-6">
        {/* Device selector */}
        {devices.length === 0 ? (
          <div className="text-center py-12">
            <p className="text-gray-600 mb-4">No devices registered yet.</p>
            <a href="/admin" className="bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700">Add a Device</a>
          </div>
        ) : (
          <div className="mb-6">
            <select
              value={selectedDevice?.id ?? ''}
              onChange={e => {
                const d = devices.find(dev => dev.id === e.target.value)
                setSelectedDevice(d ?? null)
              }}
              className="w-full max-w-xs rounded-md border border-gray-300 px-3 py-2 bg-white"
            >
              <option value="">-- Choose a device --</option>
              {devices.map(d => (
                <option key={d.id} value={d.id}>{d.device_name}</option>
              ))}
            </select>
          </div>
        )}

        {selectedDevice && (
          <>
            {/* VC Cards row */}
            <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4 mb-6">
              {vcCards.map(card => (
                <VCDashboardCard
                  key={card.vcIdx}
                  vcName={card.vcName}
                  voltage={card.voltage}
                  current={card.current}
                  power={card.power}
                  socPct={card.socPct}
                  batteryCapacity={card.batteryCapacity}
                  relayOn={card.relayOn}
                  online={selectedDevice.is_online ?? false}
                />
              ))}
            </div>

            {/* Quick stats row */}
            <QuickStatsRow
              latestReading={latestReading?.payload ?? null}
              deviceChannels={deviceChannels}
              relayOn={relayOn}
            />

            {/* Power history chart */}
            <div className="mb-6">
              <PowerHistoryChart deviceKey={selectedDevice.device_key} />
            </div>

            {/* Relay control */}
            {selectedDevice.device_type === 'power-monitor' && (
              <RelayControl deviceKey={selectedDevice.device_key} />
            )}
          </>
        )}
      </main>
    </div>
  )
}