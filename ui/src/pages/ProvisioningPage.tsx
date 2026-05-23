import { useState } from 'react'

const SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b'
const CMD_UUID = 'c01afdfc-3cbe-4c26-a1e8-8c71a5f6f2a4'
const RESP_UUID = 'd8a7b56a-3f64-4fb6-a123-8d2e5c7a9b01'

interface BleCommand {
  cmd: string
  [key: string]: unknown
}

export default function ProvisioningPage() {
  const [step, setStep] = useState(0) // 0=disconnected, 1=scanning, 2=connected, 3=wifi set, 4=supabase set, 5=done
  const [error, setError] = useState('')
  const [device, setDevice] = useState<BluetoothDevice | null>(null)
  const [ssid, setSsid] = useState('')
  const [pass, setPass] = useState('')
  const [supabaseUrl, setSupabaseUrl] = useState('')
  const [serviceRoleKey, setServiceRoleKey] = useState('')
  const [deviceKey, setDeviceKey] = useState('')
  const [wifiNetworks, setWifiNetworks] = useState<string[]>([])
  const [progress, setProgress] = useState('')

  async function connectBLE() {
    setError('')
    setStep(1)
    try {
      const d = await navigator.bluetooth.requestDevice({
        filters: [{ name: 'PowerMonitor' }],
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

  async function sendCommand(cmd: BLECommand): Promise<unknown> {
    if (!device?.gatt) throw new Error('Not connected')
    const service = await device.gatt.getPrimaryService(SERVICE_UUID)
    const cmdChar = await service.getCharacteristic(CMD_UUID)
    const respChar = await service.getCharacteristic(RESP_UUID)

    return new Promise((resolve, reject) => {
      const handler = (e: Event) => {
        const val = (e.target as BluetoothRemoteGATTCharacteristic).value
        if (!val) return
        const text = new TextDecoder().decode(val)
        try {
          const parsed = JSON.parse(text)
          respChar.removeEventListener('characteristicvaluechanged', handler)
          resolve(parsed)
        } catch {
          reject(new Error('Invalid JSON response'))
        }
      }
      respChar.addEventListener('characteristicvaluechanged', handler)
      cmdChar.writeValue(new TextEncoder().encode(JSON.stringify(cmd)))
      setTimeout(() => reject(new Error('BLE command timeout (10s)')), 10000)
    })
  }

  async function setWiFi() {
    setError('')
    setProgress('Setting WiFi...')
    try {
      const resp = await sendCommand({ cmd: 'set_wifi', ssid, pass, pin: 123456 }) as { ok?: boolean; error?: string }
      if (!resp?.ok) throw new Error(resp?.error ?? 'Failed to set WiFi')
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
        service_role_key: serviceRoleKey,
        device_key: deviceKey,
        pin: 123456,
      }) as { ok?: boolean; error?: string }
      if (!resp?.ok) throw new Error(resp?.error ?? 'Failed to set Supabase')
      setStep(5)
      setProgress('Device provisioned successfully!')
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'Supabase setup failed')
    }
  }

  return (
    <div className="min-h-screen bg-gray-100 flex items-center justify-center p-4">
      <div className="bg-white rounded-lg shadow-lg w-full max-w-lg p-6">
        <h1 className="text-2xl font-bold mb-2 text-center">BLE Device Provisioning</h1>
        <p className="text-gray-600 text-center mb-6">Step {Math.max(1, step)} of 5</p>

        {/* Progress bar */}
        <div className="w-full bg-gray-200 rounded-full h-2 mb-6">
          <div className="bg-blue-600 h-2 rounded-full transition-all" style={{ width: `${(step / 5) * 100}%` }} />
        </div>

        {error && <div className="bg-red-50 text-red-700 p-3 rounded mb-4 text-sm">{error}</div>}
        {progress && <div className="bg-blue-50 text-blue-700 p-3 rounded mb-4 text-sm">{progress}</div>}

        {step === 0 && (
          <div className="text-center">
            <p className="text-gray-600 mb-6">
              Connect to your PowerMonitor device via Bluetooth to configure WiFi and Supabase.
            </p>
            <button onClick={connectBLE} className="bg-blue-600 text-white px-6 py-3 rounded-lg hover:bg-blue-700 font-medium">
              Scan for PowerMonitor
            </button>
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
              <label className="block text-sm font-medium text-gray-700 mb-1">Service Role Key</label>
              <input value={serviceRoleKey} onChange={e => setServiceRoleKey(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2 font-mono text-xs" placeholder="eyJhbG..." />
            </div>
            <div>
              <label className="block text-sm font-medium text-gray-700 mb-1">Device Key</label>
              <input value={deviceKey} onChange={e => setDeviceKey(e.target.value)}
                className="w-full rounded-md border border-gray-300 px-3 py-2 font-mono text-xs" placeholder="xxxxxxxx-xxxx-..." />
            </div>
            <button onClick={setSupabase}
              disabled={!supabaseUrl.trim() || !serviceRoleKey.trim() || !deviceKey.trim()}
              className="w-full bg-blue-600 text-white py-2 rounded hover:bg-blue-700 disabled:opacity-50">
              Save Supabase
            </button>
          </div>
        )}

        {step === 5 && (
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

        <div className="mt-6 text-center text-sm text-gray-500">
          Default BLE PIN: <code className="bg-gray-100 px-1 rounded">123456</code>
        </div>
      </div>
    </div>
  )
}