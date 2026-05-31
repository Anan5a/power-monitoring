import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'
import type { RelayState } from '../lib/types'

interface Props {
  deviceKey: string
}

export default function RelayControl({ deviceKey }: Props) {
  const [relays, setRelays] = useState<RelayState[]>([])
  const relayChannelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)

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

    const relayChannel = supabase
      .channel(`relay-state-control-${deviceKey}`)
      .on('postgres_changes', {
        event: '*',
        schema: 'public',
        table: 'relay_states',
        filter: `device_key=eq.${deviceKey}`,
      }, (payload) => {
        const r = payload.new as RelayState
        setRelays(prev => {
          const idx = prev.findIndex(rel => rel.id === r.id)
          if (idx >= 0) {
            const next = [...prev]
            next[idx] = r
            return next
          }
          return prev
        })
      })
      .subscribe()
    relayChannelRef.current = relayChannel
    return () => {
      if (relayChannelRef.current) {
        supabase.removeChannel(relayChannelRef.current)
        relayChannelRef.current = null
      }
    }
  }, [deviceKey])

  async function toggle(relay: RelayState) {
    const newState = !relay.is_energized
    await supabase.from('settings_commands').insert({
      device_key: deviceKey,
      cmd_type: 'set_relay',
      payload: {
        idx: relay.relay_index,
        is_energized: newState,
        active_high: relay.active_high ?? true,
        enabled: true,
        overcurrent_A: 0,
        undervoltage_V: 0,
        soc_low_pct: 0,
        soc_high_pct: 100,
        trip_delay_ms: 500,
        reset_delay_ms: 5000,
      },
      status: 'pending',
    })
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