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