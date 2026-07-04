import type { Store } from 'jotai/vanilla/store'
import { supabase } from '../../lib/supabase'
import { relayStatesAtomFamily } from '../atoms'
import type { RelayState } from '../../lib/types'
import { setRelayEnergized } from '../../lib/deviceCommands'

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
  try {
    await setRelayEnergized(deviceKey, relay.relay_index, newState, relay.active_high ?? true)
  } catch {
    // Revert on error
    store.set(relayStatesAtomFamily(deviceKey), (prev) =>
      prev.map(r => r.id === relay.id ? { ...r, is_energized: relay.is_energized } : r),
    )
  }
}
