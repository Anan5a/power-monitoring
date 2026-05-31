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
  ble_pin?: string
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
  active_high?: boolean
  last_tripped_at: string | null
}

export type BatteryChemistry = 'lead_acid' | 'lipol' | 'liion' | 'nimh' | 'lifepo4' | 'agm' | 'fla'

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

export interface BatteryConfig {
  channel?: number
  capacity_mAh: number
  initial_soc_pct: number
}

export interface BatteryProfile {
  channel: number
  name: string
  chemistry: BatteryChemistry
  system_voltage: number
  capacity_mAh: number
  initial_soc_pct: number
  cell_count: number
  full_voltage: number
  cutoff_voltage: number
  float_voltage: number
}

export interface RelayRule {
  channel: number
  overcurrent_A: number
  undervoltage_V: number
  soc_low_pct: number
  soc_high_pct: number
  trip_delay_ms: number
  reset_delay_ms: number
  gpio_pin?: number
  active_high: boolean
  enabled: boolean
  is_energized?: boolean
}

export interface VirtualChannelConfig {
  voltage_src: number
  voltage_idx: number
  current_src: number
  current_idx: number
}

export interface ChannelGroup {
  group_id: number
  name: string
  icon: number
  channel_mask: number
}

export interface ChannelCalibration {
  volt_offset_mv: number[]
  volt_gain: number[]
  curr_offset_ma: number[]
  curr_gain: number[]
}

export interface DeviceChannels {
  device_key: string
  channel_groups: ChannelGroup[]
  channel_names: ChannelName[]
  battery_profiles: BatteryProfile[]
  channel_calibration?: ChannelCalibration
  virtual_channels?: VirtualChannelConfig[]
}