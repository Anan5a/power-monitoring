/// <reference types="vite/client" />
import { useState } from 'react'
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

  async function connectBLE() {
    setError('')
    setStep(1)
    try {
      const d = await navigator.bluetooth.requestDevice({
        // Show all BLE devices — the 128-bit service UUID is in scan response
        // which not all browsers match during passive scan. User picks by name.
        acceptAllDevices: true,
        optionalServices: [SERVICE_UUID],
      })
      if (!d.gatt) throw new Error('GATT not available')
      await d.gatt.connect()
      setDevice(d)
      setStep(2)
      setProgress('Connected to PowerMonitor')
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Connection failed')
      setStep(0)
    }
  }

  async function sendCommand(cmd: BleCommand): Promise<unknown> {
    if (!device?.gatt) throw new Error('Not connected')
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
          <div className="text-center">
            <p className="text-gray-600 mb-6">
              Connect to your PowerMonitor device via Bluetooth to configure WiFi and Supabase.
            </p>
            <button onClick={connectBLE} className="bg-blue-600 text-white px-6 py-3 rounded-lg hover:bg-blue-700 font-medium">
              Scan for Bluetooth Devices
            </button>
            <p className="text-xs text-gray-500 mt-2">Look for <b>PowerMonitor</b> in the list</p>
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