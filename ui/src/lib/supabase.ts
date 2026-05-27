/// <reference types="vite/client" />
import { createClient } from '@supabase/supabase-js'
import type { DeviceChannels } from './types'

export const supabase = createClient(
  import.meta.env.VITE_SUPABASE_URL as string,
  import.meta.env.VITE_SUPABASE_ANON_KEY as string
)

export async function fetchDeviceChannels(deviceKey: string): Promise<DeviceChannels | null> {
  const { data, error } = await supabase
    .from('device_channels')
    .select('*')
    .eq('device_key', deviceKey)
    .maybeSingle()
  if (error || !data) return null
  return data as DeviceChannels
}