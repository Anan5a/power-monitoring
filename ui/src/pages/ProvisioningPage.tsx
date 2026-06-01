/// <reference types="vite/client" />
import { useState, useEffect } from 'react'
import { supabase } from '../lib/supabase'

const SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b'
const CMD_UUID = 'c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4'
const RESP_UUID = 'd8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01'

interface BleCommand {
  cmd: string
  [key: string]: unknown
}

export default function ProvisioningPage() {
  const [step, setStep] = useState(0) // 0=disconnected, 1=scanning, 2=connected, 3=wifi set, 4=supabase set, 5=calibration, 6=virtual channels, 7=done
  const [error, setError] = useState('')
  const [device, setDevice] = useState<BluetoothDevice | null>(null)
  const [ssid, setSsid] = useState('')
  const [pass, setPass] = useState('')
  const [supabaseUrl, setSupabaseUrl] = useState('')
  const [anonKey, setAnonKey] = useState('')
  const [deviceApiKey, setDeviceApiKey] = useState('')
  const [deviceKey, setDeviceKey] = useState('')
  const [progress, setProgress] = useState('')
  const [calChannel, setCalChannel] = useState(0)
  const [calibration, setCalibration] = useState<Record<string, number>>({})
  const [storedPin, setStoredPin] = useState('123456')
  const [virtualChannels, setVirtualChannels] = useState<Array<{ voltage_src: number; voltage_idx: number; current_src: number; current_idx: number } | null>>([null, null, null, null])
  const [vcSaving, setVcSaving] = useState(false)
  const [wifiCmd, setWifiCmd] = useState({ssid:'', pass:''})
  const [calCmd, setCalCmd] = useState({channel:0, type:0, value:0})
  const [invCmd, setInvCmd] = useState({channel:0, invert:false})
  const [coulombCh, setCoulombCh] = useState(0)
  const [batCmd, setBatCmd] = useState({channel:0, capacity:0, soc:100})
  const [chnCmd, setChnCmd] = useState({channel:0, name:''})

  // Listen for unexpected BLE disconnects and auto-reconnect once
  useEffect(() => {
    if (!device) return
    const onDisconnect = () => {
      console.warn('[BLE] GATT disconnected unexpectedly')
      setProgress('Bluetooth disconnected — tap Scan to reconnect')
      setStep(0)
      setDevice(null)
    }
    device.addEventListener('gattserverdisconnected', onDisconnect)
    return () => device.removeEventListener('gattserverdisconnected', onDisconnect)
  }, [device])

  async function connectBLE() {
    setError('')
    setStep(1)
    try {
      const d = await navigator.bluetooth.requestDevice({
        // Name is now in primary advertisement (fits within 31 bytes)
        filters: [{ name: 'PowerMonitor' }],
        optionalServices: [SERVICE_UUID],
      })
      if (!d.gatt) throw new Error('GATT not available')
      await d.gatt.connect()
      setDevice(d)
      setStep(0)
      setProgress('Connected to PowerMonitor')
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Connection failed')
      setStep(0)
    }
  }

  async function sendCommand(cmd: BleCommand): Promise<unknown> {
    if (!device?.gatt) throw new Error('Not connected')

    // Reconnect once if the GATT server dropped (common on mobile / Windows)
    if (!device.gatt.connected) {
      setProgress('Reconnecting Bluetooth...')
      try {
        await device.gatt.connect()
      } catch {
        throw new Error('Reconnection failed — scan again')
      }
    }

    const service = await device.gatt.getPrimaryService(SERVICE_UUID)
    const cmdChar = await service.getCharacteristic(CMD_UUID)
    const respChar = await service.getCharacteristic(RESP_UUID)

    // Must start notifications before we can receive characteristicvaluechanged events
    await respChar.startNotifications()

    return new Promise((resolve, reject) => {
      const handler = (e: Event) => {
        const val = (e.target as BluetoothRemoteGATTCharacteristic).value
        if (!val) return
        const text = new TextDecoder().decode(val)
        try {
          const parsed = JSON.parse(text)
          respChar.removeEventListener('characteristicvaluechanged', handler)
          respChar.stopNotifications().catch(() => {}) // best-effort cleanup
          resolve(parsed)
        } catch {
          respChar.removeEventListener('characteristicvaluechanged', handler)
          respChar.stopNotifications().catch(() => {})
          reject(new Error('Invalid JSON response'))
        }
      }
      respChar.addEventListener('characteristicvaluechanged', handler)
      cmdChar.writeValue(new TextEncoder().encode(JSON.stringify(cmd)))
      setTimeout(() => {
        respChar.removeEventListener('characteristicvaluechanged', handler)
        respChar.stopNotifications().catch(() => {})
        reject(new Error('BLE command timeout (10s) — check BLE connection and PIN'))
      }, 10000)
    })
  }

  async function setWiFi() {
    setError('')
    setProgress('Setting WiFi...')
    try {
      const resp = await sendCommand({ cmd: 'set_wifi', ssid, pass, pin: storedPin }) as { ok?: boolean; error?: string }
      if (!resp?.ok) {
        setError(resp?.error ?? 'Failed to set WiFi')
        return
      }
      setStep(3)
      setProgress('WiFi configured successfully')
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'WiFi setup failed')
    }
  }

  async function setSupabase() {
    setError('')
    setProgress('Configuring Supabase...')
    try {
      const resp = await sendCommand({
        cmd: 'set_supabase',
        url: supabaseUrl,
        anon_key: anonKey,
        api_key: deviceApiKey,
        device_key: deviceKey,
        pin: storedPin,
      }) as { ok?: boolean; error?: string }
      if (!resp?.ok) {
        setError(resp?.error ?? 'Failed to set Supabase')
        return
      }
      setStep(5)
      setProgress('Device provisioned successfully!')
      // Fetch the device's stored PIN from Supabase (may have been set on a prior config)
      try {
        const { data } = await supabase
          .from('devices')
          .select('ble_pin')
          .eq('device_key', deviceKey)
          .maybeSingle()
        if (data?.ble_pin) setStoredPin(data.ble_pin)
      } catch { /* use stored default if fetch fails */ }
      loadCalibration(0)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Supabase setup failed')
    }
  }

  async function loadCalibration(ch: number) {
    try {
      const resp = await sendCommand({ cmd: 'get_calibration', channel: ch, pin: storedPin }) as {
        ok?: boolean; volt_offset_mv?: number; volt_gain?: number; curr_offset_ma?: number; curr_gain?: number
      }
      if (resp?.ok) {
        setCalibration({
          volt_offset_mv: resp.volt_offset_mv ?? 0,
          volt_gain: resp.volt_gain ?? 1,
          curr_offset_ma: resp.curr_offset_ma ?? 0,
          curr_gain: resp.curr_gain ?? 1,
        })
      }
    } catch { /* ignore */ }
  }

  async function loadVirtualChannel(ch: number) {
    try {
      const resp = await sendCommand({ cmd: 'get_virtual_channel', channel: ch, pin: storedPin }) as {
        ok?: boolean; voltage_src?: number; voltage_idx?: number; current_src?: number; current_idx?: number
      }
      if (resp?.ok) {
        setVirtualChannels(prev => {
          const next = [...prev]
          next[ch] = {
            voltage_src: resp.voltage_src ?? 0,
            voltage_idx: resp.voltage_idx ?? 0,
            current_src: resp.current_src ?? 0,
            current_idx: resp.current_idx ?? 0,
          }
          return next
        })
      }
    } catch { /* ignore */ }
  }

  async function saveVirtualChannel(ch: number) {
    const vc = virtualChannels[ch]
    if (!vc) return
    setVcSaving(true)
    setError('')
    try {
      const resp = await sendCommand({
        cmd: 'set_virtual_channel',
        channel: ch,
        voltage_src: vc.voltage_src,
        voltage_idx: vc.voltage_idx,
        current_src: vc.current_src,
        current_idx: vc.current_idx,
        pin: storedPin,
      }) as { ok?: boolean; error?: string }
      if (!resp?.ok) { setError(resp?.error ?? 'Save failed'); return }
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Virtual channel save failed')
    } finally {
      setVcSaving(false)
    }
  }

  async function saveCalibration(ch: number, type: number, value: number) {
    setError('')
    try {
      const resp = await sendCommand({ cmd: 'set_calibration', channel: ch, type, value, pin: storedPin }) as { ok?: boolean; error?: string }
      if (!resp?.ok) { setError(resp?.error ?? 'Save failed'); return }
      await loadCalibration(ch)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Calibration save failed')
    }
  }

  async function resetChannelCalibration(ch: number) {
    setError('')
    try {
      const resp = await sendCommand({ cmd: 'reset_calibration', channel: ch, pin: storedPin }) as { ok?: boolean; error?: string }
      if (!resp?.ok) { setError(resp?.error ?? 'Reset failed'); return }
      await loadCalibration(ch)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Calibration reset failed')
    }
  }

  return (
    <div className="min-h-screen bg-gray-100 flex items-center justify-center p-4">
      <div className="bg-white rounded-lg shadow-lg w-full max-w-lg p-6">
        <h1 className="text-2xl font-bold mb-2 text-center">BLE Device Provisioning</h1>
        <p className="text-gray-600 text-center mb-6">Step {Math.max(1, step)} of 6</p>

        {/* Progress bar */}
        <div className="w-full bg-gray-200 rounded-full h-2 mb-6">
          <div className="bg-blue-600 h-2 rounded-full transition-all" style={{ width: `${(step / 6) * 100}%` }} />
        </div>

        {error && <div className="bg-red-50 text-red-700 p-3 rounded mb-4 text-sm">{error}</div>}
        {progress && <div className="bg-blue-50 text-blue-700 p-3 rounded mb-4 text-sm">{progress}</div>}

        {step === 0 && (
          <div className="space-y-4">
            <div className="text-center mb-4">
              <p className="text-gray-600 text-sm mb-3">
                Send commands to your PowerMonitor — no setup required.
              </p>
              <button onClick={connectBLE} className="bg-blue-600 text-white px-6 py-2 rounded-lg hover:bg-blue-700 font-medium">
                {device ? 'Reconnect BLE' : 'Connect BLE Device'}
              </button>
            </div>

            {device && (
              <div className="border-t pt-4">
                <div className="flex items-center justify-between mb-3">
                  <span className="text-sm font-medium text-green-700">
                    Connected: {device.name || 'PowerMonitor'}
                  </span>
                  <button onClick={() => { device.gatt?.disconnect(); setDevice(null); setStep(0); setProgress(''); }}
                    className="text-xs text-red-600 hover:underline">Disconnect</button>
                </div>

                <div className="grid grid-cols-2 gap-3">

                  {/* set_wifi */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">set_wifi</div>
                    <input value={wifiCmd.ssid} onChange={e => setWifiCmd(c => ({...c, ssid: e.target.value}))}
                      className="w-full rounded border border-gray-300 px-2 py-1 text-xs" placeholder="SSID" />
                    <input type="password" value={wifiCmd.pass} onChange={e => setWifiCmd(c => ({...c, pass: e.target.value}))}
                      className="w-full rounded border border-gray-300 px-2 py-1 text-xs" placeholder="Password" />
                    <button onClick={() => sendCommand({cmd:'set_wifi', ssid: wifiCmd.ssid, pass: wifiCmd.pass, pin: storedPin}).then(() => setWifiCmd({ssid:'', pass:''}))}
                      disabled={!wifiCmd.ssid.trim()}
                      className="w-full bg-blue-600 text-white text-xs py-1 rounded disabled:opacity-50">Send</button>
                  </div>

                  {/* set_calibration */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">set_calibration</div>
                    <div className="flex gap-1">
                      <select value={calCmd.channel} onChange={e => setCalCmd(c => ({...c, channel: Number(e.target.value)}))}
                        className="flex-1 rounded border border-gray-300 px-1 py-0.5 text-xs bg-white">
                        {[0,1,2].map(i => <option key={i} value={i}>CH{i}</option>)}
                      </select>
                      <select value={calCmd.type} onChange={e => setCalCmd(c => ({...c, type: Number(e.target.value)}))}
                        className="flex-1 rounded border border-gray-300 px-1 py-0.5 text-xs bg-white">
                        <option value={0}>V offset</option>
                        <option value={1}>V gain</option>
                        <option value={2}>I offset</option>
                        <option value={3}>I gain</option>
                      </select>
                      <input type="number" step="any" value={calCmd.value} onChange={e => setCalCmd(c => ({...c, value: Number(e.target.value)}))}
                        className="w-16 rounded border border-gray-300 px-1 py-0.5 text-xs" placeholder="0" />
                    </div>
                    <button onClick={() => sendCommand({cmd:'set_calibration', channel: calCmd.channel, type: calCmd.type, value: calCmd.value, pin: storedPin})}
                      className="w-full bg-blue-600 text-white text-xs py-1 rounded">Send</button>
                  </div>

                  {/* set_invert_curr */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">set_invert_curr</div>
                    <div className="flex items-center gap-2">
                      <select value={invCmd.channel} onChange={e => setInvCmd(c => ({...c, channel: Number(e.target.value)}))}
                        className="flex-1 rounded border border-gray-300 px-1 py-0.5 text-xs bg-white">
                        {[0,1,2].map(i => <option key={i} value={i}>CH{i}</option>)}
                      </select>
                      <label className="flex items-center gap-1 text-xs">
                        <input type="checkbox" checked={invCmd.invert} onChange={e => setInvCmd(c => ({...c, invert: e.target.checked}))}
                          className="w-4 h-4" />
                        Invert
                      </label>
                    </div>
                    <button onClick={() => sendCommand({cmd:'set_invert_curr', channel: invCmd.channel, invert: invCmd.invert, pin: storedPin})}
                      className="w-full bg-blue-600 text-white text-xs py-1 rounded">Send</button>
                  </div>

                  {/* reset_coulomb */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">reset_coulomb</div>
                    <select value={coulombCh} onChange={e => setCoulombCh(Number(e.target.value))}
                      className="w-full rounded border border-gray-300 px-1 py-0.5 text-xs bg-white mb-2">
                      {[0,1,2,3].map(i => <option key={i} value={i}>Channel {i}</option>)}
                    </select>
                    <button onClick={() => sendCommand({cmd:'reset_coulomb', channel: coulombCh, pin: storedPin})}
                      className="w-full bg-orange-500 text-white text-xs py-1 rounded">Reset</button>
                  </div>

                  {/* set_battery */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">set_battery</div>
                    <select value={batCmd.channel} onChange={e => setBatCmd(c => ({...c, channel: Number(e.target.value)}))}
                      className="w-full rounded border border-gray-300 px-1 py-0.5 text-xs bg-white mb-1">
                      {[0,1,2,3].map(i => <option key={i} value={i}>Channel {i}</option>)}
                    </select>
                    <input type="number" step="any" value={batCmd.capacity} onChange={e => setBatCmd(c => ({...c, capacity: Number(e.target.value)}))}
                      className="w-full rounded border border-gray-300 px-2 py-1 text-xs" placeholder="Capacity mAh" />
                    <input type="number" step="any" value={batCmd.soc} onChange={e => setBatCmd(c => ({...c, soc: Number(e.target.value)}))}
                      className="w-full rounded border border-gray-300 px-2 py-1 text-xs" placeholder="Initial SoC %" />
                    <button onClick={() => sendCommand({cmd:'set_battery', channel: batCmd.channel, capacity_mAh: batCmd.capacity, initial_soc_pct: batCmd.soc, pin: storedPin})}
                      className="w-full bg-blue-600 text-white text-xs py-1 rounded">Send</button>
                  </div>

                  {/* set_channel_name */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">set_channel_name</div>
                    <select value={chnCmd.channel} onChange={e => setChnCmd(c => ({...c, channel: Number(e.target.value)}))}
                      className="w-full rounded border border-gray-300 px-1 py-0.5 text-xs bg-white mb-1">
                      {[0,1,2,3].map(i => <option key={i} value={i}>Channel {i}</option>)}
                    </select>
                    <input value={chnCmd.name} onChange={e => setChnCmd(c => ({...c, name: e.target.value}))}
                      className="w-full rounded border border-gray-300 px-2 py-1 text-xs" placeholder="Display name" />
                    <button onClick={() => sendCommand({cmd:'set_channel_name', channel: chnCmd.channel, name: chnCmd.name, pin: storedPin})}
                      className="w-full bg-blue-600 text-white text-xs py-1 rounded">Send</button>
                  </div>

                  {/* calibrate_baseline */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">calibrate_baseline</div>
                    <p className="text-xs text-gray-500">16 samples, ~16s. Device continues normal operation.</p>
                    <button onClick={() => sendCommand({cmd:'calibrate_baseline', pin: storedPin})}
                      className="w-full bg-indigo-600 text-white text-xs py-1 rounded">Start</button>
                  </div>

                  {/* reboot */}
                  <div className="border rounded p-3 space-y-2">
                    <div className="font-medium text-sm text-gray-800">reboot</div>
                    <button onClick={() => { if (confirm('Reboot device now?')) sendCommand({cmd:'reboot', pin: storedPin}) }}
                      className="w-full bg-yellow-500 text-white text-xs py-1 rounded">Reboot</button>
                  </div>

                  {/* factory_reset */}
                  <div className="border rounded p-3 space-y-2 col-span-2">
                    <div className="font-medium text-sm text-red-700">factory_reset</div>
                    <p className="text-xs text-gray-500">Erases ALL settings — WiFi, Supabase, calibration, coulomb counters.</p>
                    <button onClick={() => { if (confirm('Wipe ALL settings? This cannot be undone.')) sendCommand({cmd:'factory_reset', pin: storedPin}) }}
                      className="w-full bg-red-600 text-white text-xs py-1 rounded">Factory Reset</button>
                  </div>

                </div>
              </div>
            )}
          </div>
        )}

        {step === 1 && <div className="text-center text-gray-600 py-8">Scanning for devices...</div>}

        {step === 2 && (
          <div className="space-y-4">
            <h2 className="font-semibold text-lg">Configure WiFi</h2>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">WiFi SSID</label>
              <input value={ssid} onChange={e => setSsid(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2" placeholder="Your WiFi name" />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">WiFi Password</label>
              <input type="password" value={pass} onChange={e => setPass(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2" placeholder="WiFi password" />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">BLE PIN <span className="text-gray-400 font-normal">(default: 123456)</span></label>
              <input type="password" value={storedPin} onChange={e => setStoredPin(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2" placeholder="123456" maxLength={8} />
            </div>
            <button onClick={setWiFi}
              disabled={!ssid.trim()}
              className="w-full bg-blue-600 text-white py-2 rounded hover:bg-blue-700 disabled:opacity-50">
              Save WiFi
            </button>
          </div>
        )}

        {step === 3 && (
          <div className="space-y-4">
            <h2 className="font-semibold text-lg">Configure Supabase</h2>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Supabase URL</label>
              <input value={supabaseUrl} onChange={e => setSupabaseUrl(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2" placeholder="https://xxxx.supabase.co" />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Anon / Publishable Key</label>
              <input value={anonKey} onChange={e => setAnonKey(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2 font-mono text-xs" placeholder="eyJhbG..." />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Device API Key (UUID)</label>
              <input value={deviceApiKey} onChange={e => setDeviceApiKey(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2 font-mono text-xs" placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" />
              <p className="text-xs text-gray-500 mt-1">From Supabase devices table, not sb_secret_...</p>
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Device Key</label>
              <input value={deviceKey} onChange={e => setDeviceKey(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2 font-mono text-xs" placeholder="mydevice1" />
            </div>
            <button onClick={setSupabase}
              disabled={!supabaseUrl.trim() || !anonKey.trim() || !deviceApiKey.trim() || !deviceKey.trim()}
              className="w-full bg-blue-600 text-white py-2 rounded hover:bg-blue-700 disabled:opacity-50">
              Save Supabase
            </button>
          </div>
        )}

        {step === 5 && (
          <div className="space-y-4">
            <h2 className="font-semibold text-lg">Step 5: Channel Calibration</h2>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Channel</label>
              <select value={calChannel} onChange={e => { setCalChannel(Number(e.target.value)); loadCalibration(Number(e.target.value)) }}
                className="w-full rounded-md border border-gray-300 px-3 py-2 bg-white">
                <option value={0}>Ch0 — Battery Bank</option>
                <option value={1}>Ch1 — Solar PV</option>
                <option value={2}>Ch2 — Output</option>
              </select>
            </div>
            <div className="bg-gray-50 rounded p-3 space-y-2 text-sm">
              <div className="flex items-center gap-2">
                <span className="w-36 text-gray-600">volt_offset_mv</span>
                <input type="number" step="0.01" value={calibration.volt_offset_mv ?? ''} onChange={e => setCalibration(c => ({ ...c, volt_offset_mv: Number(e.target.value) }))}
                  className="flex-1 rounded-md border border-gray-300 px-2 py-1" />
                <button onClick={() => saveCalibration(calChannel, 0, calibration.volt_offset_mv ?? 0)}
                  className="bg-blue-600 text-white px-3 py-1 rounded hover:bg-blue-700 text-xs">Save</button>
              </div>
              <div className="flex items-center gap-2">
                <span className="w-36 text-gray-600">volt_gain</span>
                <input type="number" step="0.001" value={calibration.volt_gain ?? ''} onChange={e => setCalibration(c => ({ ...c, volt_gain: Number(e.target.value) }))}
                  className="flex-1 rounded-md border border-gray-300 px-2 py-1" />
                <button onClick={() => saveCalibration(calChannel, 1, calibration.volt_gain ?? 1)}
                  className="bg-blue-600 text-white px-3 py-1 rounded hover:bg-blue-700 text-xs">Save</button>
              </div>
              <div className="flex items-center gap-2">
                <span className="w-36 text-gray-600">curr_offset_ma</span>
                <input type="number" step="0.01" value={calibration.curr_offset_ma ?? ''} onChange={e => setCalibration(c => ({ ...c, curr_offset_ma: Number(e.target.value) }))}
                  className="flex-1 rounded-md border border-gray-300 px-2 py-1" />
                <button onClick={() => saveCalibration(calChannel, 2, calibration.curr_offset_ma ?? 0)}
                  className="bg-blue-600 text-white px-3 py-1 rounded hover:bg-blue-700 text-xs">Save</button>
              </div>
              <div className="flex items-center gap-2">
                <span className="w-36 text-gray-600">curr_gain</span>
                <input type="number" step="0.001" value={calibration.curr_gain ?? ''} onChange={e => setCalibration(c => ({ ...c, curr_gain: Number(e.target.value) }))}
                  className="flex-1 rounded-md border border-gray-300 px-2 py-1" />
                <button onClick={() => saveCalibration(calChannel, 3, calibration.curr_gain ?? 1)}
                  className="bg-blue-600 text-white px-3 py-1 rounded hover:bg-blue-700 text-xs">Save</button>
              </div>
              <div className="pt-2 border-t border-gray-200">
                <button onClick={() => resetChannelCalibration(calChannel)}
                  className="text-red-600 hover:text-red-700 text-xs">Reset this channel</button>
              </div>
            </div>
            <button onClick={() => {
                // Load all virtual channel configs then go to step 6
                for (let ch = 0; ch < 4; ch++) loadVirtualChannel(ch)
                setStep(6)
                setProgress('')
              }}
              className="w-full bg-gray-200 text-gray-700 py-2 rounded hover:bg-gray-300">
              Skip / Continue →
            </button>
          </div>
        )}

        {step === 6 && (
          <div className="space-y-4">
            <h2 className="font-semibold text-lg">Step 6: Virtual Channel Mapping</h2>
            <p className="text-sm text-gray-600">Configure which physical sensor feeds each virtual channel's voltage and current.</p>
            <div className="space-y-3 max-h-80 overflow-y-auto">
              {[0, 1, 2, 3].map(ch => {
                const vc = virtualChannels[ch]
                return (
                  <div key={ch} className="border rounded p-3">
                    <div className="font-medium text-sm mb-2">Channel {ch}</div>
                    <div className="grid grid-cols-2 gap-2 text-xs">
                      <div>
                        <span className="text-gray-500">Voltage source</span>
                        <select value={vc?.voltage_src ?? 0}
                          onChange={e => setVirtualChannels(prev => {
                            const next = [...prev]
                            next[ch] = { ...(next[ch] ?? { voltage_src: 0, voltage_idx: 0, current_src: 0, current_idx: 0 }), voltage_src: Number(e.target.value) }
                            return next
                          })}
                          className="w-full rounded border-gray-300 px-2 py-1 mt-1">
                          <option value={0}>None</option>
                          <option value={1}>INA3221 Voltage (0x42)</option>
                          <option value={2}>INA3221 Current (0x40)</option>
                          <option value={3}>INA226</option>
                          <option value={4}>ADS1115</option>
                        </select>
                      </div>
                      <div>
                        <span className="text-gray-500">Voltage index</span>
                        <select value={vc?.voltage_idx ?? 0}
                          onChange={e => setVirtualChannels(prev => {
                            const next = [...prev]
                            next[ch] = { ...(next[ch] ?? { voltage_src: 0, voltage_idx: 0, current_src: 0, current_idx: 0 }), voltage_idx: Number(e.target.value) }
                            return next
                          })}
                          className="w-full rounded border-gray-300 px-2 py-1 mt-1">
                          {[0, 1, 2, 3].map(i => <option key={i} value={i}>{i}</option>)}
                        </select>
                      </div>
                      <div>
                        <span className="text-gray-500">Current source</span>
                        <select value={vc?.current_src ?? 0}
                          onChange={e => setVirtualChannels(prev => {
                            const next = [...prev]
                            next[ch] = { ...(next[ch] ?? { voltage_src: 0, voltage_idx: 0, current_src: 0, current_idx: 0 }), current_src: Number(e.target.value) }
                            return next
                          })}
                          className="w-full rounded border-gray-300 px-2 py-1 mt-1">
                          <option value={0}>None</option>
                          <option value={2}>INA3221 Current (0x40)</option>
                          <option value={3}>INA226</option>
                        </select>
                      </div>
                      <div>
                        <span className="text-gray-500">Current index</span>
                        <select value={vc?.current_idx ?? 0}
                          onChange={e => setVirtualChannels(prev => {
                            const next = [...prev]
                            next[ch] = { ...(next[ch] ?? { voltage_src: 0, voltage_idx: 0, current_src: 0, current_idx: 0 }), current_idx: Number(e.target.value) }
                            return next
                          })}
                          className="w-full rounded border-gray-300 px-2 py-1 mt-1">
                          {[0, 1, 2, 3].map(i => <option key={i} value={i}>{i}</option>)}
                        </select>
                      </div>
                    </div>
                    {vc && (
                      <div className="mt-2 text-xs text-gray-400">
                        → V=src{vc.voltage_src}:idx{vc.voltage_idx} , I=src{vc.current_src}:idx{vc.current_idx}
                      </div>
                    )}
                    <button onClick={() => saveVirtualChannel(ch)}
                      disabled={vcSaving || !vc}
                      className="mt-2 text-xs bg-blue-600 text-white px-3 py-1 rounded hover:bg-blue-700 disabled:opacity-50">
                      {vcSaving ? 'Saving...' : 'Save'}
                    </button>
                  </div>
                )
              })}
            </div>
            <button onClick={() => { setStep(7); setProgress('Done! Go to Dashboard'); }}
              className="w-full bg-gray-200 text-gray-700 py-2 rounded hover:bg-gray-300">
              Skip / Continue →
            </button>
          </div>
        )}

        {step === 7 && (
          <div className="text-center">
            <div className="text-6xl mb-4">✓</div>
            <h2 className="text-xl font-bold text-green-600 mb-2">Device Provisioned!</h2>
            <p className="text-gray-600 mb-4">
              Your PowerMonitor is configured and ready to send data.
            </p>
            <div className="bg-gray-100 rounded p-3 text-left text-xs font-mono mb-4">
              Device Key: {deviceKey}
            </div>
            <a href="/dashboard" className="text-blue-600 hover:underline">Go to Dashboard →</a>
          </div>
        )}

              </div>
    </div>
  )
}