import type { Store } from 'jotai/vanilla/store'
import { fetchDeviceChannels } from '../../lib/supabase'
import { deviceChannelsAtomFamily } from '../atoms'
import type { DeviceChannels } from '../../lib/types'

const cache = new Map<string, DeviceChannels>()

export async function loadChannels(store: Store, deviceKey: string): Promise<DeviceChannels | null> {
  if (cache.has(deviceKey)) {
    const cached = cache.get(deviceKey)!
    store.set(deviceChannelsAtomFamily(deviceKey), cached)
    // eslint-disable-next-line no-console
    console.debug('[channelsService] cache hit for', deviceKey, cached)
    return cached
  }
  const data = await fetchDeviceChannels(deviceKey)
  // eslint-disable-next-line no-console
  console.debug('[channelsService] fetched for', deviceKey, data)
  if (data) {
    cache.set(deviceKey, data)
    store.set(deviceChannelsAtomFamily(deviceKey), data)
  }
  return data
}

export function invalidateChannels(deviceKey: string) {
  cache.delete(deviceKey)
}