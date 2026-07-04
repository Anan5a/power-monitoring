// deviceCommands.ts
// =============================================================================
// Thin client that mirrors the BLE command surface for web consumers.
//
// Every BLE command in docs/API.md has a corresponding TypeScript function
// here. Each function inserts a row into the Supabase `settings_commands`
// table; the device polls that table via the `claim_settings_command` RPC
// (every 5 s on the network task) and applies the command with no PIN.
//
// Reads (`get_*`) are NOT issued through this module — they pull from
// `telemetry_live`, `battery_profiles`, `relay_states`, and other Supabase
// tables populated by the device's regular publish path. Use the existing
// hooks in lib/useTelemetryInit.ts and lib/useDevicesLoader.ts for those.
//
// This module is the single source of truth for the BLE→Supabase command
// mapping. New BLE commands MUST be added here; pages must NOT insert
// settings_commands rows directly.
// =============================================================================

import { supabase } from './supabase'

// ── Switch / relay ───────────────────────────────────────────────────────────

export type SwitchType = 0 | 1 | 2 | 3 | 4  // RELAY, MOSFET_LOW, MOSFET_HIGH, SSR, EXPANDER
export type SwitchConditionKind = 'overcurrent' | 'undervoltage' | 'soc_low' | 'soc_high' | 'channel_above' | 'channel_below' | 'schedule_window' | 'disabled'
export type SwitchConditionOp = 'gt' | 'lt' | 'gte' | 'lte' | 'eq'
export type SwitchLogic = 'and' | 'or'

export interface SwitchCondition {
  kind: SwitchConditionKind
  op: SwitchConditionOp
  value: number
  ref_channel?: number
  schedule_mask?: number[]  // 7 × 24-bit array for schedule windows
}

export interface SwitchChannelConfig {
  idx: number
  type: SwitchType
  gpio_pin: number
  active_high: boolean
  enabled: boolean
  name?: string
}

export interface SwitchRuleConfig {
  switch_idx: number
  channel: number
  conditions: SwitchCondition[]
  logic: SwitchLogic
  min_conditions: number
  trip_delay_ms: number
  reset_delay_ms: number
  hysteresis: number
  enabled: boolean
}

/**
 * Set a switch channel + rule. Accepts the new list-shape; the firmware
 * also accepts the legacy flat-shape for backward compatibility, but new
 * web code should always send the list-shape.
 */
export async function setSwitch(deviceKey: string, channel: SwitchChannelConfig, rule: SwitchRuleConfig) {
  return enqueue(deviceKey, 'set_switch', { ...channel, ...rule })
}

export async function getSwitch(deviceKey: string, idx: number) {
  // Reads are not pushed via settings_commands; the device populates relay_states
  // and we read from there. The web hook layer should expose a getSwitch helper
  // that queries the relay_states table directly. Stub here for API symmetry.
  return enqueue(deviceKey, 'get_switch', { idx })
}

// ── Battery ──────────────────────────────────────────────────────────────────

// Matches BatteryChemistryEnum in firmware/include/battery_profile.h:
//   BAT_CHEM_LEAD_ACID = 0, LIION, LFP, LIPO, NICD, NIMH, CUSTOM
// Do NOT conflate with the legacy BatteryChemistry in lib/types.ts (which
// uses different strings for an older schema). The web surface here is
// canonical for the new command set; legacy pages still use the old type.
export type BatteryChemistry = 'lead_acid' | 'liion' | 'lfp' | 'lipo' | 'nicd' | 'nimh' | 'custom'

export interface BatteryChemistryProfile {
  id: number
  name: string
  chemistry: BatteryChemistry
  nominal_voltage: number
  rated_capacity_Ah: number
  c_rating: number
  cutoff_voltage: number
  float_voltage: number
  charge_efficiency: number
  cycle_life_rated: number
  min_soc_pct: number
  max_soc_pct: number
}

export interface BatteryBinding {
  channel: number
  profile_id: number
}

/** Bind a logical channel to a battery profile. */
export async function setBattery(deviceKey: string, channel: number, profile_id: number) {
  return enqueue(deviceKey, 'set_battery', { channel, profile_id })
}

/** Create or update a custom battery profile (or overwrite a built-in). */
export async function setBatteryProfile(deviceKey: string, profile: BatteryChemistryProfile) {
  return enqueue(deviceKey, 'set_battery_profile', profile as unknown as Record<string, unknown>)
}

/** Delete a custom battery profile (built-ins cannot be deleted). */
export async function deleteBatteryProfile(deviceKey: string, id: number) {
  return enqueue(deviceKey, 'delete_battery_profile', { id })
}

/** Reset a channel's cycle accumulator + capacity test state. */
export async function resetBattery(deviceKey: string, channel: number) {
  return enqueue(deviceKey, 'reset_battery', { channel })
}

// ── Capacity test ────────────────────────────────────────────────────────────

export type CapacityTestMode = 'manual' | 'automated'

export interface CapacityTestStartParams {
  channel: number
  mode: CapacityTestMode
  load_switch_idx?: number  // required for automated
  cutoff_v?: number          // required for automated
}

export async function capacityTestStart(deviceKey: string, params: CapacityTestStartParams) {
  return enqueue(deviceKey, 'capacity_test', { action: 'start', ...params })
}

export async function capacityTestStop(deviceKey: string, channel: number) {
  return enqueue(deviceKey, 'capacity_test', { action: 'stop', channel })
}

export async function capacityTestStatus(deviceKey: string, channel: number) {
  return enqueue(deviceKey, 'capacity_test', { action: 'status', channel })
}

// ── Channel configuration ────────────────────────────────────────────────────

export interface ChannelCalibration {
  volt_offset_mv: number[]
  volt_gain: number[]
  curr_offset_ma: number[]
  curr_gain: number[]
}

export async function setCalibration(deviceKey: string, channel: number, type: number, value: number) {
  return enqueue(deviceKey, 'set_calibration', { channel, type, value })
}

export async function resetCalibration(deviceKey: string, channel: number) {
  return enqueue(deviceKey, 'reset_calibration', { channel })
}

export async function setInvertCurrent(deviceKey: string, channel: number, invert: boolean) {
  return enqueue(deviceKey, 'set_invert_curr', { channel, invert })
}

export async function setShunt(deviceKey: string, channel: number, ohms: number) {
  return enqueue(deviceKey, 'set_shunt', { channel, ohms })
}

export async function setVoltRatio(deviceKey: string, channel: number, ratio: number) {
  return enqueue(deviceKey, 'set_volt_ratio', { channel, ratio })
}

export async function setResistors(deviceKey: string, channel: number, r_high: number, r_low: number) {
  return enqueue(deviceKey, 'set_resistors', { channel, r_high, r_low })
}

export async function calibrateBaseline(deviceKey: string) {
  return enqueue(deviceKey, 'calibrate_baseline', {})
}

export async function resetCoulomb(deviceKey: string, channel: number) {
  return enqueue(deviceKey, 'reset_coulomb', { channel })
}

export async function setChannelName(deviceKey: string, channel: number, name: string) {
  return enqueue(deviceKey, 'set_channel_name', { channel, name })
}

// ── Connectivity / provisioning ──────────────────────────────────────────────

export async function setWifi(deviceKey: string, ssid: string, pass: string) {
  return enqueue(deviceKey, 'set_wifi', { ssid, pass })
}

export async function setMqtt(deviceKey: string, broker: string, port: number, topic: string, user?: string, pass?: string) {
  return enqueue(deviceKey, 'set_mqtt', { broker, port, topic, user, pass })
}

export async function setHttp(deviceKey: string, url: string, token?: string) {
  return enqueue(deviceKey, 'set_http', { url, token })
}

export async function setSupabase(deviceKey: string, url: string, anon_key: string) {
  return enqueue(deviceKey, 'set_supabase', { url, anon_key })
}

// ── Misc ─────────────────────────────────────────────────────────────────────

export async function setPin(deviceKey: string, old_pin: string, new_pin: string) {
  return enqueue(deviceKey, 'set_pin', { old_pin, new_pin })
}

export async function reboot(deviceKey: string, pin: string) {
  return enqueue(deviceKey, 'reboot', { pin })
}

export async function factoryReset(deviceKey: string, pin: string) {
  return enqueue(deviceKey, 'factory_reset', { pin })
}

// ── Internal ─────────────────────────────────────────────────────────────────

async function enqueue(deviceKey: string, cmd_type: string, payload: Record<string, unknown>) {
  if (!deviceKey) throw new Error('deviceKey is required')
  const { data, error } = await supabase
    .from('settings_commands')
    .insert({ device_key: deviceKey, cmd_type, payload, status: 'pending' })
  if (error) {
    console.error(`[deviceCommands] ${cmd_type} failed:`, error)
    throw error
  }
  return data
}
