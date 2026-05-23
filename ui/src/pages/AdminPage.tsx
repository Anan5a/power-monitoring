import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'

export default function AdminPage() {
  const [devices, setDevices] = useState<Device[]>([])
  const [loading, setLoading] = useState(true)
  const [newDeviceName, setNewDeviceName] = useState('')
  const [newDeviceType, setNewDeviceType] = useState('generic')
  const [creating, setCreating] = useState(false)

  useEffect(() => {
    loadDevices()
  }, [])

  async function loadDevices() {
    const { data } = await supabase.from('devices').select('*').order('created_at', { ascending: false })
    if (data) setDevices(data)
    setLoading(false)
  }

  async function createDevice() {
    if (!newDeviceName.trim()) return
    setCreating(true)
    const deviceKey = crypto.randomUUID()
    const { data: { user } } = await supabase.auth.getUser()
    if (!user) return

    const { error } = await supabase.from('devices').insert({
      device_name: newDeviceName.trim(),
      device_type: newDeviceType,
      device_key: deviceKey,
      user_id: user.id,
    })

    if (!error) {
      setNewDeviceName('')
      loadDevices()
    }
    setCreating(false)
  }

  async function deleteDevice(device: Device) {
    if (!confirm(`Delete "${device.device_name}"? This cannot be undone.`)) return
    await supabase.from('devices').delete().eq('id', device.id)
    loadDevices()
  }

  async function copyKey(key: string) {
    await navigator.clipboard.writeText(key)
  }

  return (
    <div className="min-h-screen bg-gray-100">
      <header className="bg-white shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <h1 className="text-xl font-bold text-gray-800">Admin</h1>
          <div className="flex gap-4">
            <a href="/dashboard" className="text-blue-600 hover:underline text-sm">Dashboard</a>
            <button onClick={() => supabase.auth.signOut()} className="text-gray-600 hover:text-gray-800 text-sm">
              Sign Out
            </button>
          </div>
        </div>
      </header>

      <main className="max-w-4xl mx-auto px-4 py-6">
        <div className="bg-white rounded-lg shadow p-6 mb-6">
          <h2 className="text-lg font-semibold mb-4">Register New Device</h2>
          <div className="flex gap-3">
            <input
              type="text"
              placeholder="Device name"
              value={newDeviceName}
              onChange={e => setNewDeviceName(e.target.value)}
              className="flex-1 rounded-md border border-gray-300 px-3 py-2"
            />
            <select
              value={newDeviceType}
              onChange={e => setNewDeviceType(e.target.value)}
              className="rounded-md border border-gray-300 px-3 py-2 bg-white"
            >
              <option value="generic">Generic</option>
              <option value="power-monitor">Power Monitor</option>
            </select>
            <button
              onClick={createDevice}
              disabled={creating || !newDeviceName.trim()}
              className="bg-blue-600 text-white px-4 py-2 rounded hover:bg-blue-700 disabled:opacity-50"
            >
              {creating ? 'Creating...' : 'Add Device'}
            </button>
          </div>
        </div>

        <div className="bg-white rounded-lg shadow">
          <div className="px-6 py-4 border-b border-gray-200">
            <h2 className="text-lg font-semibold">Your Devices</h2>
          </div>
          {loading ? (
            <div className="p-6 text-center text-gray-500">Loading...</div>
          ) : devices.length === 0 ? (
            <div className="p-6 text-center text-gray-500">No devices yet.</div>
          ) : (
            <table className="w-full">
              <thead className="bg-gray-50">
                <tr>
                  <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase">Name</th>
                  <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase">Type</th>
                  <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase">Status</th>
                  <th className="px-6 py-3 text-left text-xs font-medium text-gray-500 uppercase">Device Key</th>
                  <th className="px-6 py-3"></th>
                </tr>
              </thead>
              <tbody className="divide-y divide-gray-200">
                {devices.map(device => (
                  <tr key={device.id} className="hover:bg-gray-50">
                    <td className="px-6 py-4 font-medium text-gray-800">{device.device_name}</td>
                    <td className="px-6 py-4 text-gray-600">{device.device_type}</td>
                    <td className="px-6 py-4">
                      <span className={`inline-flex items-center px-2 py-1 rounded-full text-xs font-medium ${
                        device.is_online ? 'bg-green-100 text-green-800' : 'bg-gray-100 text-gray-600'
                      }`}>
                        {device.is_online ? 'Online' : 'Offline'}
                      </span>
                    </td>
                    <td className="px-6 py-4">
                      <button
                        onClick={() => copyKey(device.device_key)}
                        className="text-blue-600 hover:underline text-xs font-mono"
                        title="Click to copy"
                      >
                        {device.device_key.slice(0, 8)}...
                      </button>
                    </td>
                    <td className="px-6 py-4 text-right">
                      <button
                        onClick={() => deleteDevice(device)}
                        className="text-red-600 hover:text-red-800 text-sm"
                      >
                        Delete
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </main>
    </div>
  )
}