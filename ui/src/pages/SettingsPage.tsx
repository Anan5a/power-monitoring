import { useState, useEffect } from 'react'
import { supabase } from '../lib/supabase'
import type { Device, DeviceChannels } from '../lib/types'

const EMPTY_CHANNELS: DeviceChannels = {
  device_key: '',
  channel_groups: [],
  channel_names: [],
  battery_profiles: [],
}

export default function SettingsPage() {
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedKey, setSelectedKey] = useState<string>('')
  const [deviceChannels, setDeviceChannels] = useState<DeviceChannels>(EMPTY_CHANNELS)
  const [activeTab, setActiveTab] = useState('network')
  const [message, setMessage] = useState('')
  const [loadingDevices, setLoadingDevices] = useState(true)

  // Command form states per tab
  const [wifi, setWifi] = useState({ ssid: '', pass: '' })
  const [mqtt, setMqtt] = useState({ broker: '', port: '1883', topic: '' })
  const [http, setHttp] = useState({ url: '', token: '', enabled: true })
  const [supabaseCfg, setSupabaseCfg] = useState({ url: '', anon_key: '', api_key: '', device_key: '' })

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
    if (!selectedKey) { setDeviceChannels(EMPTY_CHANNELS); return }
    async function loadChannels() {
      const { data } = await supabase
        .from('device_channels')
        .select('*')
        .eq('device_key', selectedKey)
        .maybeSingle()
      if (data) setDeviceChannels(data as DeviceChannels)
      else setDeviceChannels(EMPTY_CHANNELS)
    }
    loadChannels()
  }, [selectedKey])

  async function sendCommand(cmd_type: string, payload: Record<string, unknown>) {
    if (!selectedKey) return
    const { error } = await supabase.from('settings_commands').insert({
      device_key: selectedKey,
      cmd_type,
      payload,
      status: 'pending',
    })
    if (error) {
      setMessage(`Error: ${error.message}`)
    } else {
      setMessage(`Command queued: ${cmd_type}`)
      setTimeout(() => setMessage(''), 3000)
    }
  }

  async function saveWifi() {
    await sendCommand('set_wifi', { ssid: wifi.ssid, pass: wifi.pass })
  }
  async function saveMqtt() {
    await sendCommand('set_mqtt', { broker: mqtt.broker, port: parseInt(mqtt.port) || 1883, topic: mqtt.topic })
  }
  async function saveHttp() {
    await sendCommand('set_http', { url: http.url, token: http.token, enabled: http.enabled })
  }
  async function saveSupabase() {
    await sendCommand('set_supabase', {
      url: supabaseCfg.url,
      anon_key: supabaseCfg.anon_key,
      api_key: supabaseCfg.api_key,
      device_key: supabaseCfg.device_key,
    })
  }

  const tabs = ['network', 'supabase', 'relays', 'batteries', 'calibration', 'virtual', 'groups', 'names', 'system']

  if (loadingDevices) {
    return <div className="flex items-center justify-center h-screen text-gray-500">Loading...</div>
  }

  return (
    <div className="min-h-screen bg-gray-100">
      <header className="bg-white shadow-sm">
        <div className="max-w-5xl mx-auto px-4 py-4 flex items-center justify-between">
          <h1 className="text-xl font-bold text-gray-800">Settings</h1>
          <a href="/dashboard" className="text-blue-600 hover:underline text-sm">← Dashboard</a>
        </div>
      </header>

      <main className="max-w-5xl mx-auto px-4 py-6">
        {/* Device selector */}
        <div className="mb-6">
          <label className="block text-sm font-medium text-gray-700 mb-2">Device</label>
          <select
            value={selectedKey}
            onChange={e => setSelectedKey(e.target.value)}
            className="w-full max-w-xs rounded-md border border-gray-300 px-3 py-2 bg-white"
          >
            <option value="">-- Choose a device --</option>
            {devices.map(d => (
              <option key={d.id} value={d.device_key}>{d.device_name} ({d.device_key})</option>
            ))}
          </select>
        </div>

        {selectedKey && (
          <>
            {/* Tab bar */}
            <div className="flex gap-1 mb-6 flex-wrap">
              {tabs.map(t => (
                <button key={t}
                  onClick={() => setActiveTab(t)}
                  className={`px-3 py-1.5 rounded text-sm font-medium capitalize ${
                    activeTab === t ? 'bg-blue-600 text-white' : 'bg-white text-gray-600 hover:bg-gray-50'
                  }`}
                >{t}</button>
              ))}
            </div>

            {message && (
              <div className="mb-4 p-3 rounded text-sm bg-blue-50 text-blue-700">{message}</div>
            )}

            {/* Network tab */}
            {activeTab === 'network' && (
              <div className="bg-white rounded-lg shadow p-6 space-y-6">
                <div>
                  <h3 className="font-semibold mb-3">WiFi</h3>
                  <div className="grid grid-cols-2 gap-4">
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">SSID</label>
                      <input value={wifi.ssid} onChange={e => setWifi(w => ({ ...w, ssid: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="WiFi name" />
                    </div>
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">Password</label>
                      <input type="password" value={wifi.pass} onChange={e => setWifi(w => ({ ...w, pass: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="Password" />
                    </div>
                  </div>
                  <button onClick={saveWifi} className="mt-2 bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700 text-sm">
                    Save WiFi
                  </button>
                </div>

                <div>
                  <h3 className="font-semibold mb-3">MQTT</h3>
                  <div className="grid grid-cols-3 gap-4">
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">Broker</label>
                      <input value={mqtt.broker} onChange={e => setMqtt(m => ({ ...m, broker: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="192.168.1.100" />
                    </div>
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">Port</label>
                      <input value={mqtt.port} onChange={e => setMqtt(m => ({ ...m, port: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="1883" />
                    </div>
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">Topic</label>
                      <input value={mqtt.topic} onChange={e => setMqtt(m => ({ ...m, topic: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="power-monitor/data" />
                    </div>
                  </div>
                  <button onClick={saveMqtt} className="mt-2 bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700 text-sm">
                    Save MQTT
                  </button>
                </div>

                <div>
                  <h3 className="font-semibold mb-3">HTTP Endpoint</h3>
                  <div className="grid grid-cols-2 gap-4">
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">URL</label>
                      <input value={http.url} onChange={e => setHttp(h => ({ ...h, url: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="https://..." />
                    </div>
                    <div>
                      <label className="block text-sm text-gray-600 mb-1">Token</label>
                      <input value={http.token} onChange={e => setHttp(h => ({ ...h, token: e.target.value }))}
                        className="w-full rounded border border-gray-300 px-3 py-2" placeholder="Bearer token" />
                    </div>
                  </div>
                  <label className="flex items-center gap-2 mt-2">
                    <input type="checkbox" checked={http.enabled}
                      onChange={e => setHttp(h => ({ ...h, enabled: e.target.checked }))}
                      className="rounded" />
                    <span className="text-sm">Enabled</span>
                  </label>
                  <button onClick={saveHttp} className="mt-2 bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700 text-sm">
                    Save HTTP
                  </button>
                </div>
              </div>
            )}

            {/* Supabase tab */}
            {activeTab === 'supabase' && (
              <div className="bg-white rounded-lg shadow p-6 space-y-4">
                <h3 className="font-semibold">Supabase Configuration</h3>
                <div className="grid grid-cols-2 gap-4">
                  <div>
                    <label className="block text-sm text-gray-600 mb-1">URL</label>
                    <input value={supabaseCfg.url} onChange={e => setSupabaseCfg(c => ({ ...c, url: e.target.value }))}
                      className="w-full rounded border border-gray-300 px-3 py-2" placeholder="https://..." />
                  </div>
                  <div>
                    <label className="block text-sm text-gray-600 mb-1">Anon Key</label>
                    <input value={supabaseCfg.anon_key} onChange={e => setSupabaseCfg(c => ({ ...c, anon_key: e.target.value }))}
                      className="w-full rounded border border-gray-300 px-3 py-2" placeholder="eyJ..." />
                  </div>
                  <div>
                    <label className="block text-sm text-gray-600 mb-1">API Key</label>
                    <input value={supabaseCfg.api_key} onChange={e => setSupabaseCfg(c => ({ ...c, api_key: e.target.value }))}
                      className="w-full rounded border border-gray-300 px-3 py-2" placeholder="eyJ..." />
                  </div>
                  <div>
                    <label className="block text-sm text-gray-600 mb-1">Device Key</label>
                    <input value={supabaseCfg.device_key} onChange={e => setSupabaseCfg(c => ({ ...c, device_key: e.target.value }))}
                      className="w-full rounded border border-gray-300 px-3 py-2" placeholder="my-device-1" />
                  </div>
                </div>
                <button onClick={saveSupabase} className="bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700 text-sm">
                  Save Supabase
                </button>
              </div>
            )}

            {/* Calibration tab */}
            {activeTab === 'calibration' && (
              <CalibrationTab deviceChannels={deviceChannels} onSave={(ch, type, value) =>
                sendCommand('set_calibration', { channel: ch, type, value })
              } />
            )}

            {/* Virtual channels tab */}
            {activeTab === 'virtual' && (
              <VirtualChannelsTab deviceChannels={deviceChannels} onSave={(ch, vc) =>
                sendCommand('set_virtual_channel', { channel: ch, ...vc })
              } />
            )}

            {/* Channel names tab */}
            {activeTab === 'names' && (
              <ChannelNamesTab deviceChannels={deviceChannels} onSave={(ch, name) =>
                sendCommand('set_channel_name', { channel: ch, name })
              } />
            )}

            {/* Channel groups tab */}
            {activeTab === 'groups' && (
              <ChannelGroupsTab deviceChannels={deviceChannels} onSave={(idx, cg) =>
                sendCommand('set_channel_group', { group_id: idx, ...cg })
              } />
            )}

            {/* Batteries tab */}
            {activeTab === 'batteries' && (
              <BatteriesTab onSave={(ch, bat) =>
                sendCommand('set_battery', { channel: ch, ...bat })
              } />
            )}

            {/* System tab */}
            {activeTab === 'system' && (
              <div className="bg-white rounded-lg shadow p-6 space-y-4">
                <h3 className="font-semibold">System</h3>
                <button onClick={() => sendCommand('reboot', {})}
                  className="bg-yellow-500 text-white px-4 py-2 rounded hover:bg-yellow-600 text-sm mr-2">
                  Reboot Device
                </button>
                <button onClick={() => {
                  if (confirm('Factory reset will erase ALL settings. Continue?'))
                    sendCommand('factory_reset', {})
                }}
                  className="bg-red-600 text-white px-4 py-2 rounded hover:bg-red-700 text-sm">
                  Factory Reset
                </button>
                <p className="text-sm text-gray-500 mt-2">
                  Commands are queued and applied within ~30 seconds. Factory reset erases all calibration, WiFi, and Supabase settings.
                </p>
              </div>
            )}

            {/* Relays tab placeholder */}
            {activeTab === 'relays' && (
              <RelaysTab onSave={(idx, rt) =>
                sendCommand('set_relay', { idx, ...rt })
              } />
            )}
          </>
        )}
      </main>
    </div>
  )
}

function CalibrationTab({ deviceChannels, onSave }: { deviceChannels: DeviceChannels; onSave: (ch: number, type: number, value: number) => void }) {
  const cal = deviceChannels.channel_calibration
  const [vals, setVals] = useState({
    volt_offset_mv: ['0','0','0'],
    volt_gain: ['1','1','1'],
    curr_offset_ma: ['0','0','0'],
    curr_gain: ['1','1','1'],
  })
  useEffect(() => {
    if (cal) {
      setVals({
        volt_offset_mv: cal.volt_offset_mv.map(v => String(v)),
        volt_gain: cal.volt_gain.map(v => String(v)),
        curr_offset_ma: cal.curr_offset_ma.map(v => String(v)),
        curr_gain: cal.curr_gain.map(v => String(v)),
      })
    }
  }, [cal])

  const types = ['volt_offset_mv', 'volt_gain', 'curr_offset_ma', 'curr_gain']

  return (
    <div className="bg-white rounded-lg shadow p-6">
      <h3 className="font-semibold mb-4">Calibration (per channel)</h3>
      <table className="w-full text-sm">
        <thead>
          <tr className="text-left text-gray-600">
            <th className="pb-2">VC</th>
            <th className="pb-2">volt_offset_mv</th>
            <th className="pb-2">volt_gain</th>
            <th className="pb-2">curr_offset_ma</th>
            <th className="pb-2">curr_gain</th>
          </tr>
        </thead>
        <tbody>
          {[0,1,2].map(ch => (
            <tr key={ch} className="border-t">
              <td className="py-2 font-medium">CH{ch}</td>
              {types.map((type) => (
                <td key={type} className="py-2 pr-2">
                  <input
                    value={vals[type === 'volt_offset_mv' ? 'volt_offset_mv' : type === 'volt_gain' ? 'volt_gain' : type === 'curr_offset_ma' ? 'curr_offset_ma' : 'curr_gain'][ch]}
                    onChange={e => setVals(v => ({
                      ...v,
                      [type === 'volt_offset_mv' ? 'volt_offset_mv' : type === 'volt_gain' ? 'volt_gain' : type === 'curr_offset_ma' ? 'curr_offset_ma' : 'curr_gain']:
                        v[type === 'volt_offset_mv' ? 'volt_offset_mv' : type === 'volt_gain' ? 'volt_gain' : type === 'curr_offset_ma' ? 'curr_offset_ma' : 'curr_gain'].map((x, i) => i === ch ? e.target.value : x)
                    }))}
                    className="w-full rounded border border-gray-300 px-2 py-1"
                  />
                </td>
              ))}
              <td className="py-2">
                <button onClick={() => {
                  types.forEach((type, ti) => {
                    const key = type === 'volt_offset_mv' ? 'volt_offset_mv' : type === 'volt_gain' ? 'volt_gain' : type === 'curr_offset_ma' ? 'curr_offset_ma' : 'curr_gain'
                    onSave(ch, ti, parseFloat(vals[key][ch]) || 0)
                  })
                }}
                  className="text-xs bg-blue-600 text-white px-2 py-1 rounded">Save</button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}

function VirtualChannelsTab({ deviceChannels: _dc, onSave }: { deviceChannels: DeviceChannels; onSave: (ch: number, vc: object) => void }) {
  const srcOptions = [
    { value: 0, label: 'None' },
    { value: 1, label: 'INA3221 Voltage (0x42)' },
    { value: 2, label: 'INA3221 Current (0x40)' },
    { value: 3, label: 'INA226' },
    { value: 4, label: 'ADS1115' },
  ]
  const currOptions = [
    { value: 0, label: 'None' },
    { value: 2, label: 'INA3221 Current (0x40)' },
    { value: 3, label: 'INA226' },
  ]
  // Load from deviceChannels if available
  const [vcs, setVcs] = useState<Array<{ voltage_src: number; voltage_idx: number; current_src: number; current_idx: number }>>(
    Array.from({ length: 4 }, () => ({ voltage_src: 0, voltage_idx: 0, current_src: 0, current_idx: 0 }))
  )

  return (
    <div className="bg-white rounded-lg shadow p-6">
      <h3 className="font-semibold mb-4">Virtual Channel Mapping</h3>
      <div className="space-y-4">
        {[0,1,2,3].map(ch => (
          <div key={ch} className="border rounded p-3">
            <div className="font-medium mb-2">VC{ch} (Virtual Channel)</div>
            <div className="grid grid-cols-4 gap-3 text-sm">
              <div>
                <span className="text-gray-500">V Source</span>
                <select value={vcs[ch].voltage_src} onChange={e => {
                  const next = [...vcs]; next[ch] = { ...next[ch], voltage_src: Number(e.target.value) }; setVcs(next)
                }} className="w-full rounded border border-gray-300 px-2 py-1 mt-1">
                  {srcOptions.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
                </select>
              </div>
              <div>
                <span className="text-gray-500">V Index</span>
                <select value={vcs[ch].voltage_idx} onChange={e => {
                  const next = [...vcs]; next[ch] = { ...next[ch], voltage_idx: Number(e.target.value) }; setVcs(next)
                }} className="w-full rounded border border-gray-300 px-2 py-1 mt-1">
                  {[0,1,2,3].map(i => <option key={i} value={i}>{i}</option>)}
                </select>
              </div>
              <div>
                <span className="text-gray-500">I Source</span>
                <select value={vcs[ch].current_src} onChange={e => {
                  const next = [...vcs]; next[ch] = { ...next[ch], current_src: Number(e.target.value) }; setVcs(next)
                }} className="w-full rounded border border-gray-300 px-2 py-1 mt-1">
                  {currOptions.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
                </select>
              </div>
              <div>
                <span className="text-gray-500">I Index</span>
                <select value={vcs[ch].current_idx} onChange={e => {
                  const next = [...vcs]; next[ch] = { ...next[ch], current_idx: Number(e.target.value) }; setVcs(next)
                }} className="w-full rounded border border-gray-300 px-2 py-1 mt-1">
                  {[0,1,2,3].map(i => <option key={i} value={i}>{i}</option>)}
                </select>
              </div>
            </div>
            <button onClick={() => onSave(ch, vcs[ch])}
              className="mt-2 text-xs bg-blue-600 text-white px-3 py-1 rounded">Save</button>
          </div>
        ))}
      </div>
    </div>
  )
}

function ChannelNamesTab({ deviceChannels, onSave }: { deviceChannels: DeviceChannels; onSave: (ch: number, name: string) => void }) {
  const names = deviceChannels.channel_names || []
  const [vals, setVals] = useState(['', '', '', ''])
  useEffect(() => {
    [0,1,2,3].forEach(ch => {
      const n = names.find(c => c.channel === ch)
      setVals(prev => prev.map((v, i) => i === ch ? (n?.name || '') : v))
    })
  }, [names])

  return (
    <div className="bg-white rounded-lg shadow p-6">
      <h3 className="font-semibold mb-4">Channel Names</h3>
      <div className="space-y-3">
        {[0,1,2,3].map(ch => (
          <div key={ch} className="flex items-center gap-3">
            <span className="w-16 text-sm text-gray-600">VC{ch}</span>
            <input value={vals[ch]} onChange={e => setVals(v => v.map((x, i) => i === ch ? e.target.value : x))}
              className="flex-1 rounded border border-gray-300 px-3 py-2" placeholder="e.g. Solar Panel" />
            <button onClick={() => onSave(ch, vals[ch])}
              className="text-xs bg-blue-600 text-white px-3 py-1 rounded">Save</button>
          </div>
        ))}
      </div>
    </div>
  )
}

function ChannelGroupsTab({ deviceChannels, onSave }: { deviceChannels: DeviceChannels; onSave: (idx: number, cg: object) => void }) {
  const groups = deviceChannels.channel_groups || []
  const [vals, setVals] = useState([{ name: '', icon: 0, channel_mask: 0 }, { name: '', icon: 0, channel_mask: 0 }, { name: '', icon: 0, channel_mask: 0 }, { name: '', icon: 0, channel_mask: 0 }])
  useEffect(() => {
    groups.forEach(g => {
      if (g.group_id < 4) setVals(v => v.map((x, i) => i === g.group_id ? { name: g.name, icon: g.icon, channel_mask: g.channel_mask } : x))
    })
  }, [groups])

  const icons = ['☀️', '🔋', '⚡', '📟']

  return (
    <div className="bg-white rounded-lg shadow p-6">
      <h3 className="font-semibold mb-4">Channel Groups</h3>
      <div className="space-y-3">
        {[0,1,2,3].map(idx => (
          <div key={idx} className="border rounded p-3">
            <div className="flex gap-2 mb-2">
              <input value={vals[idx].name} onChange={e => setVals(v => v.map((x, i) => i === idx ? { ...x, name: e.target.value } : x))}
                className="flex-1 rounded border border-gray-300 px-2 py-1" placeholder="Group name" />
              <select value={vals[idx].icon} onChange={e => setVals(v => v.map((x, i) => i === idx ? { ...x, icon: Number(e.target.value) } : x))}
                className="rounded border border-gray-300 px-2 py-1">
                {icons.map((icon, i) => <option key={i} value={i}>{icon}</option>)}
              </select>
            </div>
            <div className="flex gap-2 items-center">
              <span className="text-sm text-gray-500">VCs:</span>
              {[0,1,2,3].map(ch => (
                <label key={ch} className="flex items-center gap-1 text-sm">
                  <input type="checkbox"
                    checked={(vals[idx].channel_mask & (1 << ch)) !== 0}
                    onChange={e => setVals(v => v.map((x, i) => i === idx ? {
                      ...x, channel_mask: e.target.checked ? x.channel_mask | (1 << ch) : x.channel_mask & ~(1 << ch)
                    } : x))}
                  />
                  VC{ch}
                </label>
              ))}
              <button onClick={() => onSave(idx, vals[idx])}
                className="ml-auto text-xs bg-blue-600 text-white px-3 py-1 rounded">Save</button>
            </div>
          </div>
        ))}
      </div>
    </div>
  )
}

function BatteriesTab({ onSave }: { onSave: (ch: number, bat: object) => void }) {
  const [vals, setVals] = useState([{ capacity_mAh: '', initial_soc_pct: '100' }, { capacity_mAh: '', initial_soc_pct: '100' }, { capacity_mAh: '', initial_soc_pct: '100' }, { capacity_mAh: '', initial_soc_pct: '100' }])
  return (
    <div className="bg-white rounded-lg shadow p-6">
      <h3 className="font-semibold mb-4">Battery Configuration (per VC)</h3>
      <p className="text-sm text-gray-500 mb-4">Set capacity for VCs with a battery. Leave at 0 (or empty) for non-battery VCs like solar or grid.</p>
      <div className="space-y-3">
        {[0,1,2,3].map(ch => (
          <div key={ch} className="border rounded p-3">
            <div className="font-medium mb-2">VC{ch}</div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="text-sm text-gray-600">Capacity (mAh, 0=disabled)</label>
                <input value={vals[ch].capacity_mAh} onChange={e => setVals(v => v.map((x, i) => i === ch ? { ...x, capacity_mAh: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-2 py-1" placeholder="5000" />
              </div>
              <div>
                <label className="text-sm text-gray-600">Initial SoC (%)</label>
                <input value={vals[ch].initial_soc_pct} onChange={e => setVals(v => v.map((x, i) => i === ch ? { ...x, initial_soc_pct: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-2 py-1" placeholder="100" />
              </div>
            </div>
            <button onClick={() => onSave(ch, {
              capacity_mAh: parseFloat(vals[ch].capacity_mAh) || 0,
              initial_soc_pct: parseFloat(vals[ch].initial_soc_pct) || 100,
            })} className="mt-2 text-xs bg-blue-600 text-white px-3 py-1 rounded">Save</button>
          </div>
        ))}
      </div>
    </div>
  )
}

function RelaysTab({ onSave }: { onSave: (idx: number, rt: object) => void }) {
  const [relays, setRelays] = useState(Array.from({ length: 4 }, () => ({
    channel: 0, overcurrent_A: '0', undervoltage_V: '0',
    soc_low_pct: '0', soc_high_pct: '0', trip_delay_ms: '1000',
    reset_delay_ms: '5000', gpio_pin: '25', active_high: false, enabled: true,
  })))

  return (
    <div className="bg-white rounded-lg shadow p-6">
      <h3 className="font-semibold mb-4">Relay Rules</h3>
      <div className="space-y-4">
        {[0,1,2,3].map(idx => (
          <div key={idx} className="border rounded p-3">
            <div className="font-medium mb-2">Relay {idx}</div>
            <div className="grid grid-cols-4 gap-2 text-sm">
              <div>
                <label className="text-xs text-gray-500">VC</label>
                <select value={relays[idx].channel} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, channel: Number(e.target.value) } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1">
                  {[0,1,2,3].map(c => <option key={c} value={c}>VC{c}</option>)}
                </select>
              </div>
              <div>
                <label className="text-xs text-gray-500">Overcurrent (A)</label>
                <input value={relays[idx].overcurrent_A} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, overcurrent_A: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1" />
              </div>
              <div>
                <label className="text-xs text-gray-500">Undervoltage (V)</label>
                <input value={relays[idx].undervoltage_V} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, undervoltage_V: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1" />
              </div>
              <div>
                <label className="text-xs text-gray-500">SoC Low (%)</label>
                <input value={relays[idx].soc_low_pct} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, soc_low_pct: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1" />
              </div>
              <div>
                <label className="text-xs text-gray-500">SoC High (%)</label>
                <input value={relays[idx].soc_high_pct} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, soc_high_pct: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1" />
              </div>
              <div>
                <label className="text-xs text-gray-500">Trip delay (ms)</label>
                <input value={relays[idx].trip_delay_ms} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, trip_delay_ms: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1" />
              </div>
              <div>
                <label className="text-xs text-gray-500">GPIO</label>
                <input value={relays[idx].gpio_pin} onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, gpio_pin: e.target.value } : x))}
                  className="w-full rounded border border-gray-300 px-1 py-1" />
              </div>
              <div className="flex items-center gap-1 pt-4">
                <input type="checkbox" checked={relays[idx].active_high}
                  onChange={e => setRelays(r => r.map((x, i) => i === idx ? { ...x, active_high: e.target.checked } : x))} />
                <span className="text-xs">Active High</span>
              </div>
            </div>
            <button onClick={() => onSave(idx, {
              channel: relays[idx].channel,
              overcurrent_A: parseFloat(relays[idx].overcurrent_A) || 0,
              undervoltage_V: parseFloat(relays[idx].undervoltage_V) || 0,
              soc_low_pct: parseFloat(relays[idx].soc_low_pct) || 0,
              soc_high_pct: parseFloat(relays[idx].soc_high_pct) || 0,
              trip_delay_ms: parseInt(relays[idx].trip_delay_ms) || 1000,
              reset_delay_ms: parseInt(relays[idx].reset_delay_ms) || 5000,
              gpio_pin: parseInt(relays[idx].gpio_pin) || 25,
              active_high: relays[idx].active_high,
              enabled: relays[idx].enabled,
            })} className="mt-2 text-xs bg-blue-600 text-white px-3 py-1 rounded">Save</button>
          </div>
        ))}
      </div>
    </div>
  )
}