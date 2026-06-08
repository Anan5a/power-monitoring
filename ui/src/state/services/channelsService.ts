import type { Store } from 'jotai/vanilla/store'
import { fetchDeviceChannels } from '../../lib/supabase'
import { deviceChannelsAtomFamily } from '../atoms'
import type { DeviceChannels } from '../../lib/types'

const cache = new Map<string, DeviceChannels>()

export async function loadChannels(store: Store, deviceKey: string): Promise<DeviceChannels | null> {
  if (cache.has(deviceKey)) {
    const cached = cache.get(deviceKey)!
    store.set(deviceChannelsAtomFamily(deviceKey), cached)
    return cached
  }
  const data = await fetchDeviceChannels(deviceKey)
  if (data) {
    cache.set(deviceKey, data)
    store.set(deviceChannelsAtomFamily(deviceKey), data)
  }
  return data
}

export function invalidateChannels(deviceKey: string) {
  cache.delete(deviceKey)
}