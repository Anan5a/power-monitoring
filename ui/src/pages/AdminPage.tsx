import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'
import DashboardLayout from '../components/DashboardLayout'
import HeaderBar from '../components/HeaderBar'

export default function AdminPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [loading, setLoading] = useState(true)
  const [newDeviceName, setNewDeviceName] = useState('')
  const [newDeviceType, setNewDeviceType] = useState('generic')
  const [creating, setCreating] = useState(false)
  const [selectedDeviceId, setSelectedDeviceId] = useState<string | null>(null)
  const [allDevices, setAllDevices] = useState<Device[]>([])

  useEffect(() => {
    loadDevices()
  }, [])

  async function loadDevices() {
    const { data } = await supabase.from('devices').select('*').order('created_at', { ascending: false })
    if (data) {
      setDevices(data)
      setAllDevices(data)
    }
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

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut(); navigate('/login') }

  const header = ({ onMenuClick }: { onMenuClick: () => void }) => (
    <HeaderBar
      devices={allDevices}
      selectedDeviceId={selectedDeviceId}
      onSelectDevice={(d) => {
        setSelectedDeviceId(d.id)
        navigate('/dashboard')
      }}
      isOnline={false}
      lastUpdated={null}
      onMenuClick={onMenuClick}
    />
  )

  return (
    <DashboardLayout
      currentPath="/admin"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      header={header}
    >
      <div className="space-y-6">
        {/* Register new device */}
        <div className="bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
          <h2 className="text-lg font-semibold mb-4 text-slate-800">Register New Device</h2>
          <div className="flex gap-3">
            <input
              type="text"
              placeholder="Device name"
              value={newDeviceName}
              onChange={e => setNewDeviceName(e.target.value)}
              className="flex-1 rounded-xl border border-slate-200 px-3 py-2 focus:outline-none focus:ring-2 focus:ring-brand-400 text-sm"
            />
            <select
              value={newDeviceType}
              onChange={e => setNewDeviceType(e.target.value)}
              className="rounded-xl border border-slate-200 px-3 py-2 bg-white text-sm focus:outline-none focus:ring-2 focus:ring-brand-400"
            >
              <option value="generic">Generic</option>
              <option value="power-monitor">Power Monitor</option>
            </select>
            <button
              onClick={createDevice}
              disabled={creating || !newDeviceName.trim()}
              className="bg-brand-600 text-white px-5 py-2 rounded-xl hover:bg-brand-700 disabled:opacity-50 text-sm font-medium transition-colors"
            >
              {creating ? 'Creating...' : 'Add Device'}
            </button>
          </div>
        </div>

        {/* Device list */}
        <div className="bg-white rounded-2xl shadow-sm border border-slate-100 overflow-hidden">
          <div className="px-6 py-4 border-b border-slate-100">
            <h2 className="text-lg font-semibold text-slate-800">Your Devices</h2>
          </div>
          {loading ? (
            <div className="p-8 text-center text-slate-400">Loading...</div>
          ) : devices.length === 0 ? (
            <div className="p-8 text-center text-slate-400">No devices yet.</div>
          ) : (
            <table className="w-full">
              <thead className="bg-slate-50">
                <tr>
                  <th className="px-6 py-3 text-left text-xs font-medium text-slate-500 uppercase tracking-wider">Name</th>
                  <th className="px-6 py-3 text-left text-xs font-medium text-slate-500 uppercase tracking-wider">Type</th>
                  <th className="px-6 py-3 text-left text-xs font-medium text-slate-500 uppercase tracking-wider">Status</th>
                  <th className="px-6 py-3 text-left text-xs font-medium text-slate-500 uppercase tracking-wider">Device Key</th>
                  <th className="px-6 py-3"></th>
                </tr>
              </thead>
              <tbody className="divide-y divide-slate-100">
                {devices.map(device => (
                  <tr key={device.id} className="hover:bg-slate-50 transition-colors">
                    <td className="px-6 py-4 font-medium text-slate-800">{device.device_name}</td>
                    <td className="px-6 py-4 text-slate-500 text-sm">{device.device_type}</td>
                    <td className="px-6 py-4">
                      <span className={`inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium ${
                        device.is_online ? 'bg-emerald-100 text-emerald-700' : 'bg-slate-100 text-slate-500'
                      }`}>
                        {device.is_online ? 'Online' : 'Offline'}
                      </span>
                    </td>
                    <td className="px-6 py-4">
                      <button
                        onClick={() => copyKey(device.device_key)}
                        className="text-brand-600 hover:underline text-xs font-mono"
                        title="Click to copy"
                      >
                        {device.device_key.slice(0, 8)}...
                      </button>
                    </td>
                    <td className="px-6 py-4 text-right">
                      <button
                        onClick={() => deleteDevice(device)}
                        className="text-red-500 hover:text-red-700 text-sm transition-colors"
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
      </div>
    </DashboardLayout>
  )
}