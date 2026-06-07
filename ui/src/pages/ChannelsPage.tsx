import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { useRealtime } from '../hooks/useRealtime'
import type { Device, DeviceChannels } from '../lib/types'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'
import type { ReactNode } from 'react'

type Tab = 'sensors' | 'virtual' | 'battery' | 'relays'

function ChannelsPageInner({
  selectedDevice,
  deviceChannels,
  activeTab,
  onTabChange,
  payload,
  relayStates,
  relayLoaded,
  onToggleRelay,
}: {
  selectedDevice: Device | null
  deviceChannels: DeviceChannels | null
  activeTab: Tab
  onTabChange: (t: Tab) => void
  payload: Record<string, number> | null
  relayStates: Array<{relay_index:number,gpio_pin:number,is_energized:boolean,active_high?:boolean,last_tripped_at?:string}>
  relayLoaded: boolean
  onToggleRelay: (idx: number) => void
}): ReactNode {
  return (
    <>
      {/* Tab bar */}
      <div className="flex gap-2 mb-6">
        {(['sensors', 'virtual', 'battery', 'relays'] as Tab[]).map(t => (
          <button
            key={t}
            onClick={() => onTabChange(t)}
            className={`px-4 py-2 rounded font-medium text-sm capitalize transition-colors ${
              activeTab === t
                ? 'bg-brand-600 text-white'
                : 'bg-white text-slate-600 hover:bg-slate-50 border border-slate-200'
            }`}
          >
            {t}
          </button>
        ))}
      </div>

      {/* Sensors tab */}
      {activeTab === 'sensors' && payload && (
        <div className="space-y-6">
          {/* INA3221 */}
          <div className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
            <div className="flex items-center gap-2 mb-4">
              <span className="px-2.5 py-1 rounded text-xs font-bold bg-amber-100 text-amber-700">INA3221</span>
              <span className="text-sm text-slate-500">0x40 current + 0x42 voltage — 3-channel each</span>
            </div>
            <div className="grid grid-cols-3 md:grid-cols-9 gap-4 text-center">
              {['v0','v1','v2','i0','i1','i2','p0','p1','p2'].map(key => {
                const fullKey = `ina3221_${key}`
                const val = payload[fullKey]
                const suffix = key.slice(-1)
                const spikeKey = `ina3221_${key.startsWith('v') ? 'v' : 'i'}${suffix}_spike`
                const stddevKey = `ina3221_${key.startsWith('v') ? 'v' : 'i'}${suffix}_stddev`
                const spikeVal = payload[spikeKey]
                const isSpike = !!(spikeVal)
                const stddevVal = payload[stddevKey]
                const stddev = typeof stddevVal === 'number' ? stddevVal : null
                const unit = key.startsWith('v') ? 'V' : key.startsWith('i') ? 'A' : 'W'
                const label = key.replace(/^v/,'V Ch').replace(/^i/,'I Ch').replace(/^p/,'P Ch')
                return (
                  <div key={key} className={`bg-slate-50 rounded-xl p-3 ${isSpike ? ' ring-1 ring-red-400' : ''}`}>
                    <div className="text-xs text-slate-400 mb-1">{label}</div>
                    <div className={`font-bold text-slate-800 text-lg ${isSpike ? 'text-red-600' : ''}`}>
                      {val != null ? val.toFixed(2) : '--'}
                    </div>
                    <div className="text-xs text-slate-400">{unit}</div>
                    {isSpike && <div className="text-xs text-red-500 font-medium mt-1">⚠ spike</div>}
                    {stddev != null && stddev > 0 && (
                      <div className="text-xs text-slate-400 mt-0.5">σ={stddev.toFixed(3)}</div>
                    )}
                  </div>
                )
              })}
            </div>
          </div>

          {/* INA226 */}
          {'ina226_v' in payload && (
          <div className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
            <div className="flex items-center gap-2 mb-4">
              <span className="px-2.5 py-1 rounded text-xs font-bold bg-emerald-100 text-emerald-700">INA226</span>
              <span className="text-sm text-slate-500">0x41 — high-side current/power monitor</span>
            </div>
            <div className="grid grid-cols-3 gap-4 text-center">
              {['ina226_v','ina226_i','ina226_p'].map(key => {
                const val = payload[key]
                const unit = key.endsWith('_v') ? 'V' : key.endsWith('_i') ? 'A' : 'W'
                const label = key.replace('ina226_','').toUpperCase()
                return (
                  <div key={key} className="bg-slate-50 rounded-xl p-3">
                    <div className="text-xs text-slate-400 mb-1">{label}</div>
                    <div className="font-bold text-slate-800 text-lg">{val != null ? val.toFixed(2) : '--'}</div>
                    <div className="text-xs text-slate-400">{unit}</div>
                  </div>
                )
              })}
            </div>
          </div>
          )}

          {/* ADS1115 */}
          {'ads1115_0' in payload && (
          <div className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
            <div className="flex items-center gap-2 mb-4">
              <span className="px-2.5 py-1 rounded text-xs font-bold bg-blue-100 text-blue-700">ADS1115</span>
              <span className="text-sm text-slate-500">0x48 — 16-bit 4-channel ADC</span>
            </div>
            <div className="grid grid-cols-4 gap-4 text-center">
              {[0,1,2,3].map(ch => {
                const key = `ads1115_${ch}`
                const val = payload[key]
                return (
                  <div key={ch} className="bg-slate-50 rounded-xl p-3">
                    <div className="text-xs text-slate-400 mb-1">CH{ch}</div>
                    <div className="font-bold text-slate-800 text-lg">{val != null ? val.toFixed(3) : '--'}</div>
                    <div className="text-xs text-slate-400">V</div>
                  </div>
                )
              })}
            </div>
          </div>
          )}
        </div>
      )}

      {/* Virtual tab */}
      {activeTab === 'virtual' && deviceChannels && (
        <div className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
          <p className="text-sm text-slate-500 mb-1">Virtual channel mapping is configured on-device via BLE provisioning.</p>
          <p className="text-sm text-slate-400 mb-4">Hardware source assignments are stored in ESP32 NVS and not re-read by the UI.</p>
          <table className="w-full text-sm">
            <thead>
              <tr className="text-left text-slate-500 border-b border-slate-100">
                <th className="pb-3">VC</th>
                <th className="pb-3">Name</th>
                <th className="pb-3">Description</th>
              </tr>
            </thead>
            <tbody>
              {[0,1,2,3].map(vcIdx => {
                const vcName = deviceChannels.channel_names?.find(cn => cn.channel === vcIdx)?.name ?? `VC${vcIdx}`
                const bp = deviceChannels.battery_profiles?.[vcIdx]
                const hasBat = bp && bp.capacity_mAh > 0
                return (
                  <tr key={vcIdx} className="border-b border-slate-50 last:border-0">
                    <td className="py-3 font-medium text-slate-700">VC{vcIdx}</td>
                    <td className="py-3 text-slate-600">VC{vcIdx} — {vcName}</td>
                    <td className="py-3 text-slate-400">
                      {hasBat ? `${bp.chemistry} · ${bp.capacity_mAh}mAh` : 'No battery configured'}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}

      {/* Battery tab */}
      {activeTab === 'battery' && deviceChannels && (
        <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
          {[0,1,2,3].map(vcIdx => {
            const bp = deviceChannels.battery_profiles?.[vcIdx]
            if (!bp?.capacity_mAh) return (
              <div key={vcIdx} className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6 opacity-60">
                <div className="font-semibold text-slate-400">VC{vcIdx} — No battery configured</div>
              </div>
            )
            const socKey = `soc_pct${vcIdx}`
            const coulombKey = `coulomb_mah${vcIdx}`
            const soc = payload?.[socKey] ?? null
            const coulomb = payload?.[coulombKey] ?? null
            const socColor = soc === null ? 'bg-slate-200' : soc > 50 ? 'bg-emerald-500' : soc > 20 ? 'bg-amber-400' : 'bg-red-500'
            const vcName = deviceChannels.channel_names?.find(cn => cn.channel === vcIdx)?.name ?? `VC${vcIdx}`
            return (
              <div key={vcIdx} className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
                <div className="flex items-center justify-between mb-3">
                  <div>
                    <div className="font-semibold text-slate-800">{vcName}</div>
                    <div className="text-xs text-slate-400">{bp.name ?? 'LiPo'} · {bp.capacity_mAh}mAh</div>
                  </div>
                  <span className="text-xs bg-blue-100 text-blue-700 px-2 py-0.5 rounded font-medium">{bp.chemistry ?? 'LiPo'}</span>
                </div>
                <div className="mb-3">
                  <div className="flex justify-between text-sm mb-1.5">
                    <span className="text-slate-500">SoC</span>
                    <span className="font-medium text-slate-700">{soc !== null ? `${soc.toFixed(0)}%` : '--'}</span>
                  </div>
                  <div className="w-full bg-slate-100 rounded-full h-2.5 overflow-hidden">
                    {soc !== null && (
                      <div className={`h-2.5 rounded-full ${socColor} transition-all`} style={{ width: `${Math.min(soc, 100)}%` }} />
                    )}
                  </div>
                </div>
                <div className="grid grid-cols-2 gap-3 text-sm">
                  <div className="bg-slate-50 rounded-xl p-3 text-center">
                    <div className="text-xs text-slate-400 mb-1">Coulomb</div>
                    <div className="font-semibold text-slate-700">{coulomb !== null ? `${coulomb.toFixed(0)} mAh` : '--'}</div>
                  </div>
                  <div className="bg-slate-50 rounded-xl p-3 text-center">
                    <div className="text-xs text-slate-400 mb-1">Capacity</div>
                    <div className="font-semibold text-slate-700">{bp.capacity_mAh}mAh</div>
                  </div>
                </div>
              </div>
            )
          })}
        </div>
      )}

      {/* Relays tab */}
      {activeTab === 'relays' && selectedDevice && (
        <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
          {[0,1,2,3].map(idx => {
            const rs = relayStates.find(r => r.relay_index === idx)
            const isOn = rs?.is_energized ?? false
            return (
              <div key={idx} className={`bg-white rounded-2xl shadow-sm border border-slate-100 border-l-4 p-5 ${isOn ? 'border-l-red-500' : 'border-l-slate-200'}`}>
                <div className="flex items-center justify-between mb-3">
                  <span className="font-bold text-slate-700">Relay {idx}</span>
                  <span className={`text-xs px-2 py-0.5 rounded font-medium ${isOn ? 'bg-red-100 text-red-700' : 'bg-slate-100 text-slate-500'}`}>
                    {isOn ? 'ON' : 'OFF'}
                  </span>
                </div>
                <div className="text-sm text-slate-500 mb-1">GPIO {rs?.gpio_pin ?? '--'}</div>
                {rs?.last_tripped_at && (
                  <div className="text-xs text-slate-400 mb-3">
                    Last tripped: {new Date(rs.last_tripped_at).toLocaleString()}
                  </div>
                )}
                <button
                  onClick={() => onToggleRelay(idx)}
                  disabled={!relayLoaded}
                  className={`w-full mt-2 py-2 rounded-lg text-sm font-medium transition-colors ${
                    isOn
                      ? 'bg-slate-100 hover:bg-slate-200 text-slate-700'
                      : 'bg-brand-600 hover:bg-brand-700 text-white'
                  } ${!relayLoaded ? 'opacity-50 cursor-not-allowed' : ''}`}
                >
                  {!relayLoaded ? 'Loading...' : isOn ? 'Turn Off' : 'Turn On'}
                </button>
              </div>
            )
          })}
        </div>
      )}
    </>
  )
}

export default function ChannelsPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [deviceChannels, setDeviceChannels] = useState<DeviceChannels | null>(null)
  const [activeTab, setActiveTab] = useState<Tab>('sensors')
  const [loadingDevices, setLoadingDevices] = useState(true)
  const [relayStates, setRelayStates] = useState<Array<{relay_index:number,gpio_pin:number,is_energized:boolean,active_high?:boolean,last_tripped_at?:string}>>([])
  const [relayLoaded, setRelayLoaded] = useState(false)

  useEffect(() => {
    supabase.auth.getSession().then(({ data: { session } }) => {
      if (!session) return
      supabase.from('devices').select('*').order('device_name').then(({ data }) => {
        if (data) setDevices(data)
        setLoadingDevices(false)
      })
    })
  }, [])

  useEffect(() => {
    if (!selectedDevice) { setDeviceChannels(null); return }
    fetchDeviceChannels(selectedDevice.device_key).then(setDeviceChannels)
  }, [selectedDevice])

  useEffect(() => {
    if (!selectedDevice) return
    supabase.from('relay_states').select('*').eq('device_key', selectedDevice.device_key).then(({ data }) => {
      if (data) setRelayStates(data as typeof relayStates)
      setRelayLoaded(true)
    })
    const relayChannel = supabase
      .channel(`channels-relays-${selectedDevice.device_key}`)
      .on('postgres_changes', {
        event: '*',
        schema: 'public',
        table: 'relay_states',
        filter: `device_key=eq.${selectedDevice.device_key}`,
      }, (payload) => {
        const r = payload.new as typeof relayStates[0]
        setRelayStates(prev => {
          const idx = prev.findIndex(rel => rel.relay_index === r.relay_index)
          if (idx >= 0) {
            const next = [...prev]; next[idx] = r; return next
          }
          return prev
        })
      })
      .subscribe()
    return () => { supabase.removeChannel(relayChannel) }
  }, [selectedDevice])

  const { latestReading } = useRealtime(selectedDevice?.device_key ?? null)
  const payload = latestReading?.payload as Record<string, number> | null

  const toggleRelay = async (idx: number) => {
    if (!selectedDevice) return
    const rs = relayStates.find(r => r.relay_index === idx)
    const isOn = rs?.is_energized ?? false
    const activeHigh = rs?.active_high ?? true
    await supabase.from('settings_commands').insert({
      device_key: selectedDevice.device_key,
      cmd_type: 'set_relay',
      payload: { idx, is_energized: !isOn, active_high: activeHigh, enabled: true, overcurrent_A: 0, undervoltage_V: 0, soc_low_pct: 0, soc_high_pct: 100, trip_delay_ms: 500, reset_delay_ms: 5000 },
      status: 'pending',
    })
  }

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut(); navigate('/login') }

  const header = ({ onMenuClick }: { onMenuClick: () => void }) => (
    <HeaderBar
      devices={devices}
      selectedDeviceId={selectedDevice?.id ?? null}
      onSelectDevice={setSelectedDevice}
      isOnline={selectedDevice?.is_online ?? false}
      onMenuClick={onMenuClick}
    />
  )

  if (loadingDevices) {
    return <div className="flex items-center justify-center h-screen text-slate-500">Loading...</div>
  }

  return (
    <DashboardLayout
      currentPath="/channels"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      header={header}
      deviceName={selectedDevice?.device_name}
    >
      {!selectedDevice ? (
        <div className="text-center py-12">
          <p className="text-slate-600 mb-4">No device selected.</p>
          <p className="text-sm text-slate-400">Select a device from the dropdown above to view channels.</p>
        </div>
      ) : devices.length === 0 ? (
        <div className="text-center py-12">
          <p className="text-slate-600">No devices registered.</p>
          <a href="/admin" className="text-brand-600 hover:underline mt-2 inline-block">Add a Device</a>
        </div>
      ) : (
        <div className="space-y-6">
          <ChannelsPageInner
            selectedDevice={selectedDevice}
            deviceChannels={deviceChannels}
            activeTab={activeTab}
            onTabChange={setActiveTab}
            payload={payload}
            relayStates={relayStates}
            relayLoaded={relayLoaded}
            onToggleRelay={toggleRelay}
          />
        </div>
      )}
    </DashboardLayout>
  )
}