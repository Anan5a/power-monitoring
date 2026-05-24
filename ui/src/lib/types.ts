export interface Profile {
  id: string
  display_name: string
  created_at: string
}

export interface Device {
  id: string
  user_id: string
  device_name: string
  device_type: string
  device_key: string
  is_online: boolean
  last_seen_at: string
  created_at: string
}

export interface DeviceProfile {
  id: number
  device_type: string
  label: string
  fields: Array<{
    key: string
    label: string
    unit: string
    chart: boolean
  }>
}

export interface TelemetryPoint {
  id: number
  device_id: string
  recorded_at: string
  payload: Record<string, number>
  metadata: Record<string, unknown>
}

export interface RelayState {
  id: number
  device_key: string
  relay_index: number
  gpio_pin: number
  is_energized: boolean
  last_tripped_at: string | null
}

export type BatteryChemistry = 'lead_acid' | 'lipol' | 'liion' | 'nimh'

export interface ChannelGroup {
  group_id: number
  name: string
  icon: number
  channel_mask: number
}

export interface ChannelName {
  channel: number
  name: string
}

export interface BatteryProfile {
  channel: number
  name: string
  chemistry: BatteryChemistry
  capacity_mAh: number
  initial_soc_pct: number
  cell_count: number
  full_voltage: number
  cutoff_voltage: number
  float_voltage: number
}

export interface DeviceChannels {
  device_key: string
  channel_groups: ChannelGroup[]
  channel_names: ChannelName[]
  battery_profiles: BatteryProfile[]
}