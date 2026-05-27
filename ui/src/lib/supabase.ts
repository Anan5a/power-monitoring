import { createClient } from '@supabase/supabase-js'
import { SUPABASE_URL, SUPABASE_ANON_KEY } from '../config'
import type { DeviceChannels } from './types'

export const supabase = createClient(SUPABASE_URL, SUPABASE_ANON_KEY)

export async function fetchDeviceChannels(deviceKey: string): Promise<DeviceChannels | null> {
  const { data, error } = await supabase
    .from('device_channels')
    .select('*')
    .eq('device_key', deviceKey)
    .single()
  if (error || !data) return null
  return data as DeviceChannels
}