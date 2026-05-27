import { useEffect, useState } from 'react'
import { supabase, fetchDeviceChannels } from '../lib/supabase'
import { useRealtime } from '../hooks/useRealtime'
import type { Device, DeviceChannels } from '../lib/types'

type Tab = 'sensors' | 'virtual' | 'battery' | 'relays'

export default function ChannelsPage() {
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useState<Device | null>(null)
  const [deviceChannels, setDeviceChannels] = useState<DeviceChannels | null>(null)
  const [activeTab, setActiveTab] = useState<Tab>('sensors')
  const [loadingDevices, setLoadingDevices] = useState(true)

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

  const { latestReading } = useRealtime(selectedDevice?.device_key ?? null)
  const payload = latestReading?.payload as Record<string, number> | null

  if (loadingDevices) return <div className="flex items-center justify-center h-screen text-gray-500">Loading...</div>

  return (
    <div className="min-h-screen bg-gray-100">
      <header className="bg-white shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <h1 className="text-xl font-bold text-gray-800">Channels</h1>
          <nav className="flex gap-6 text-sm">
            <a href="/dashboard" className="text-blue-600 hover:underline">Dashboard</a>
            <a href="/channels" className="text-blue-600 font-medium hover:underline">Channels</a>
            <a href="/settings" className="text-blue-600 hover:underline">Settings</a>
            <a href="/admin" className="text-blue-600 hover:underline">Admin</a>
            <button onClick={() => supabase.auth.signOut()} className="text-gray-500 hover:text-gray-700">Sign Out</button>
          </nav>
        </div>
      </header>

      <main className="max-w-7xl mx-auto px-4 py-6">
        {/* Device selector */}
        {devices.length === 0 ? (
          <div className="text-center py-12">
            <p className="text-gray-600">No devices registered.</p>
            <a href="/admin" className="text-blue-600 hover:underline mt-2 inline-block">Add a Device</a>
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
            {/* Tab bar */}
            <div className="flex gap-2 mb-6">
              {(['sensors', 'virtual', 'battery', 'relays'] as Tab[]).map(t => (
                <button
                  key={t}
                  onClick={() => setActiveTab(t)}
                  className={`px-4 py-2 rounded font-medium text-sm capitalize ${
                    activeTab === t ? 'bg-blue-600 text-white' : 'bg-white text-gray-600 hover:bg-gray-50'
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
                <div className="bg-white rounded-lg shadow p-6">
                  <div className="flex items-center gap-2 mb-4">
                    <span className="px-2 py-1 rounded text-xs font-bold bg-yellow-100 text-yellow-700">INA3221</span>
                    <span className="text-sm text-gray-500">0x42 — 3-channel current/voltage monitor</span>
                  </div>
                  <div className="grid grid-cols-3 md:grid-cols-9 gap-4 text-center">
                    {['v0','v1','v2','i0','i1','i2','p0','p1','p2'].map(key => {
                      const fullKey = `ina3221_${key}`
                      const val = payload[fullKey]
                      const unit = key.startsWith('v') ? 'V' : key.startsWith('i') ? 'A' : 'W'
                      const label = key.replace(/^v/,'V Ch').replace(/^i/,'I Ch').replace(/^p/,'P Ch')
                      return (
                        <div key={key} className="bg-gray-50 rounded p-2">
                          <div className="text-xs text-gray-400 mb-1">{label}</div>
                          <div className="font-semibold text-gray-700">{val != null ? val.toFixed(2) : '--'}</div>
                          <div className="text-xs text-gray-400">{unit}</div>
                        </div>
                      )
                    })}
                  </div>
                </div>

                {/* INA226 */}
                <div className="bg-white rounded-lg shadow p-6">
                  <div className="flex items-center gap-2 mb-4">
                    <span className="px-2 py-1 rounded text-xs font-bold bg-green-100 text-green-700">INA226</span>
                    <span className="text-sm text-gray-500">0x41 — high-side current/power monitor</span>
                  </div>
                  <div className="grid grid-cols-3 gap-4 text-center">
                    {['ina226_v','ina226_i','ina226_p'].map(key => {
                      const val = payload[key]
                      const unit = key.endsWith('_v') ? 'V' : key.endsWith('_i') ? 'A' : 'W'
                      const label = key.replace('ina226_','').toUpperCase()
                      return (
                        <div key={key} className="bg-gray-50 rounded p-2">
                          <div className="text-xs text-gray-400 mb-1">{label}</div>
                          <div className="font-semibold text-gray-700">{val != null ? val.toFixed(2) : '--'}</div>
                          <div className="text-xs text-gray-400">{unit}</div>
                        </div>
                      )
                    })}
                  </div>
                </div>

                {/* ADS1115 */}
                <div className="bg-white rounded-lg shadow p-6">
                  <div className="flex items-center gap-2 mb-4">
                    <span className="px-2 py-1 rounded text-xs font-bold bg-blue-100 text-blue-700">ADS1115</span>
                    <span className="text-sm text-gray-500">0x48 — 16-bit 4-channel ADC</span>
                  </div>
                  <div className="grid grid-cols-4 gap-4 text-center">
                    {[0,1,2,3].map(ch => {
                      const key = `ads1115_${ch}`
                      const val = payload[key]
                      return (
                        <div key={ch} className="bg-gray-50 rounded p-2">
                          <div className="text-xs text-gray-400 mb-1">CH{ch}</div>
                          <div className="font-semibold text-gray-700">{val != null ? val.toFixed(3) : '--'}</div>
                          <div className="text-xs text-gray-400">V</div>
                        </div>
                      )
                    })}
                  </div>
                </div>
              </div>
            )}

            {/* Virtual tab */}
            {activeTab === 'virtual' && deviceChannels && (
              <div className="bg-white rounded-lg shadow p-6">
                <p className="text-sm text-gray-500 mb-4">Virtual channel mapping: which hardware source feeds each VC.</p>
                <table className="w-full text-sm">
                  <thead>
                    <tr className="text-left text-gray-600 border-b">
                      <th className="pb-2">VC</th>
                      <th className="pb-2">Name</th>
                      <th className="pb-2">Voltage Source</th>
                      <th className="pb-2">Current Source</th>
                    </tr>
                  </thead>
                  <tbody>
                    {[0,1,2,3].map(vcIdx => {
                      const vcName = deviceChannels.channel_names?.find(cn => cn.channel === vcIdx)?.name ?? `VC${vcIdx}`
                      // Reconstruct from virtual channels if available
                      const vc = deviceChannels as unknown as Array<{voltage_src:number,voltage_idx:number,current_src:number,current_idx:number}>
                      const vSrc = vc[vcIdx]?.voltage_src ?? 0
                      const cSrc = vc[vcIdx]?.current_src ?? 0
                      const srcLabels: Record<number, {label:string, color:string}> = {
                        0: {label: 'None', color: 'bg-gray-100'},
                        1: {label: 'INA3221', color: 'bg-yellow-100'},
                        2: {label: 'INA3221 I', color: 'bg-yellow-100'},
                        3: {label: 'INA226', color: 'bg-green-100'},
                        4: {label: 'ADS1115', color: 'bg-blue-100'},
                      }
                      return (
                        <tr key={vcIdx} className="border-b last:border-0">
                          <td className="py-2 font-medium">VC{vcIdx}</td>
                          <td className="py-2">{vcName}</td>
                          <td className="py-2">
                            <span className={`px-2 py-0.5 rounded text-xs ${srcLabels[vSrc]?.color}`}>
                              {srcLabels[vSrc]?.label ?? 'Unknown'}
                              {vSrc > 0 && ` Ch${vc[vcIdx]?.voltage_idx ?? 0}`}
                            </span>
                          </td>
                          <td className="py-2">
                            <span className={`px-2 py-0.5 rounded text-xs ${srcLabels[cSrc]?.color}`}>
                              {srcLabels[cSrc]?.label ?? 'Unknown'}
                              {cSrc > 0 && ` Ch${vc[vcIdx]?.current_idx ?? 0}`}
                            </span>
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
                    <div key={vcIdx} className="bg-white rounded-lg shadow p-6 opacity-60">
                      <div className="font-semibold text-gray-500">VC{vcIdx} — No battery configured</div>
                    </div>
                  )
                  const socKey = `soc_pct${vcIdx}`
                  const coulombKey = `coulomb_mah${vcIdx}`
                  const soc = payload?.[socKey] ?? null
                  const coulomb = payload?.[coulombKey] ?? null
                  const socColor = soc === null ? 'bg-gray-200' : soc > 50 ? 'bg-green-500' : soc > 20 ? 'bg-yellow-400' : 'bg-red-500'
                  const vcName = deviceChannels.channel_names?.find(cn => cn.channel === vcIdx)?.name ?? `VC${vcIdx}`
                  return (
                    <div key={vcIdx} className="bg-white rounded-lg shadow p-6">
                      <div className="flex items-center justify-between mb-3">
                        <div>
                          <div className="font-semibold text-gray-800">{vcName}</div>
                          <div className="text-xs text-gray-400">{bp.name ?? 'LiPo'} · {bp.capacity_mAh}mAh</div>
                        </div>
                        <span className="text-xs bg-blue-100 text-blue-700 px-2 py-0.5 rounded">{bp.chemistry ?? 'LiPo'}</span>
                      </div>
                      <div className="mb-3">
                        <div className="flex justify-between text-sm mb-1">
                          <span className="text-gray-500">SoC</span>
                          <span className="font-medium">{soc !== null ? `${soc.toFixed(0)}%` : '--'}</span>
                        </div>
                        <div className="w-full bg-gray-200 rounded-full h-3">
                          {soc !== null && (
                            <div className={`h-3 rounded-full ${socColor}`} style={{ width: `${Math.min(soc, 100)}%` }} />
                          )}
                        </div>
                      </div>
                      <div className="grid grid-cols-2 gap-3 text-sm">
                        <div className="bg-gray-50 rounded p-2 text-center">
                          <div className="text-xs text-gray-400">Coulomb</div>
                          <div className="font-medium">{coulomb !== null ? `${coulomb.toFixed(0)} mAh` : '--'}</div>
                        </div>
                        <div className="bg-gray-50 rounded p-2 text-center">
                          <div className="text-xs text-gray-400">Capacity</div>
                          <div className="font-medium">{bp.capacity_mAh}mAh</div>
                        </div>
                      </div>
                    </div>
                  )
                })}
              </div>
            )}

            {/* Relays tab */}
            {activeTab === 'relays' && selectedDevice && <RelaysTab deviceKey={selectedDevice.device_key} />}
          </>
        )}
      </main>
    </div>
  )
}

function RelaysTab({ deviceKey }: { deviceKey: string }) {
  const [relayStates, setRelayStates] = useState<Array<{relay_index:number,gpio_pin:number,is_energized:boolean,last_tripped_at?:string}>>([])

  useEffect(() => {
    supabase.from('relay_states').select('*').eq('device_key', deviceKey).then(({ data }) => {
      if (data) setRelayStates(data as typeof relayStates)
    })
  }, [deviceKey])

  const toggleRelay = async (idx: number) => {
    await supabase.from('settings_commands').insert({
      device_key: deviceKey,
      cmd_type: 'set_relay',
      payload: { idx, enabled: true, overcurrent_A: 0, undervoltage_V: 0, soc_low_pct: 0, soc_high_pct: 100, trip_delay_ms: 500, reset_delay_ms: 5000, gpio_pin: 25, active_high: true, channel: 0 },
      status: 'pending',
    })
  }

  return (
    <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
      {[0,1,2,3].map(idx => {
        const rs = relayStates.find(r => r.relay_index === idx)
        const isOn = rs?.is_energized ?? false
        return (
          <div key={idx} className={`bg-white rounded-lg shadow p-6 border-l-4 ${isOn ? 'border-red-500' : 'border-gray-300'}`}>
            <div className="flex items-center justify-between mb-3">
              <span className="font-bold text-gray-700">Relay {idx}</span>
              <span className={`text-xs px-2 py-0.5 rounded font-medium ${isOn ? 'bg-red-100 text-red-700' : 'bg-gray-100 text-gray-500'}`}>
                {isOn ? 'ON' : 'OFF'}
              </span>
            </div>
            <div className="text-sm text-gray-500 mb-1">GPIO {rs?.gpio_pin ?? '--'}</div>
            {rs?.last_tripped_at && (
              <div className="text-xs text-gray-400 mb-3">
                Last tripped: {new Date(rs.last_tripped_at).toLocaleString()}
              </div>
            )}
            <button
              onClick={() => toggleRelay(idx)}
              className={`w-full mt-2 py-1 rounded text-sm font-medium ${isOn ? 'bg-gray-200 hover:bg-gray-300 text-gray-700' : 'bg-blue-600 hover:bg-blue-700 text-white'}`}
            >
              {isOn ? 'Turn Off' : 'Turn On'}
            </button>
          </div>
        )
      })}
    </div>
  )
}