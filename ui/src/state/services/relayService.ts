import type { Store } from 'jotai'
import { supabase } from '../../lib/supabase'
import { relayStatesAtomFamily } from '../atoms'
import type { RelayState } from '../../lib/types'

let currentChannel: ReturnType<typeof supabase.channel> | null = null

export async function loadRelays(store: Store, deviceKey: string) {
  const { data } = await supabase
    .from('relay_states')
    .select('*')
    .eq('device_key', deviceKey)
    .order('relay_index')
  if (data) {
    store.set(relayStatesAtomFamily(deviceKey), data as RelayState[])
  }
}

export function subscribeRelays(store: Store, deviceKey: string): () => void {
  if (currentChannel) {
    supabase.removeChannel(currentChannel)
    currentChannel = null
  }
  const channel = supabase
    .channel(`relay-state-${deviceKey}`)
    .on('postgres_changes', {
      event: '*',
      schema: 'public',
      table: 'relay_states',
      filter: `device_key=eq.${deviceKey}`,
    }, (payload) => {
      const r = payload.new as RelayState
      store.set(relayStatesAtomFamily(deviceKey), (prev) => {
        const idx = prev.findIndex(rel => rel.id === r.id)
        if (idx >= 0) {
          const next = [...prev]
          next[idx] = r
          return next
        }
        return [...prev, r].sort((a, b) => a.relay_index - b.relay_index)
      })
    })
    .subscribe()
  currentChannel = channel
  return () => {
    if (currentChannel) {
      supabase.removeChannel(currentChannel)
      currentChannel = null
    }
  }
}

export async function toggleRelay(
  store: Store,
  deviceKey: string,
  relay: RelayState,
  newState: boolean,
): Promise<void> {
  // Optimistic update
  store.set(relayStatesAtomFamily(deviceKey), (prev) =>
    prev.map(r => r.id === relay.id ? { ...r, is_energized: newState } : r),
  )
  const { error } = await supabase.from('settings_commands').insert({
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
  if (error) {
    // Revert on error
    store.set(relayStatesAtomFamily(deviceKey), (prev) =>
      prev.map(r => r.id === relay.id ? { ...r, is_energized: relay.is_energized } : r),
    )
  }
}
