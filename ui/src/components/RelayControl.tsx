import { useEffect, useState } from 'react'
import { supabase } from '../lib/supabase'
import type { RelayState } from '../lib/types'

interface Props {
  deviceKey: string
}

export default function RelayControl({ deviceKey }: Props) {
  const [relays, setRelays] = useState<RelayState[]>([])

  useEffect(() => {
    async function load() {
      const { data } = await supabase
        .from('relay_states')
        .select('*')
        .eq('device_key', deviceKey)
        .order('relay_index')
      if (data) setRelays(data)
    }
    load()
  }, [deviceKey])

  async function toggle(relay: RelayState) {
    const newState = !relay.is_energized
    await supabase
      .from('relay_states')
      .update({ is_energized: newState })
      .eq('id', relay.id)
    setRelays(relays.map(r => r.id === relay.id ? { ...r, is_energized: newState } : r))
  }

  if (relays.length === 0) return null

  return (
    <div className="bg-white rounded-lg shadow p-4">
      <h3 className="font-semibold text-gray-800 mb-3">Relay Control</h3>
      <div className="grid grid-cols-2 gap-2">
        {relays.map(relay => (
          <div key={relay.id} className="flex items-center justify-between bg-gray-50 rounded p-2">
            <span className="text-sm text-gray-600">GPIO {relay.gpio_pin}</span>
            <button
              onClick={() => toggle(relay)}
              className={`px-3 py-1 rounded text-sm font-medium ${
                relay.is_energized
                  ? 'bg-green-500 text-white'
                  : 'bg-gray-300 text-gray-700'
              }`}
            >
              {relay.is_energized ? 'ON' : 'OFF'}
            </button>
          </div>
        ))}
      </div>
    </div>
  )
}