/// <reference types="vite/client" />
import { createClient } from '@supabase/supabase-js'
import type { DeviceChannels } from './types'

const supabaseUrl = import.meta.env.VITE_SUPABASE_URL as string
const supabaseAnonKey = import.meta.env.VITE_SUPABASE_ANON_KEY as string

export const supabase = createClient(supabaseUrl, supabaseAnonKey)

export async function fetchDeviceChannels(deviceKey: string): Promise<DeviceChannels | null> {
  const { data, error } = await supabase
    .from('device_channels')
    .select('*')
    .eq('device_key', deviceKey)
    .single()
  if (error || !data) return null
  return data as DeviceChannels
}