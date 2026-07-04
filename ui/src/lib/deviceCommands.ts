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
  if (!Number.isInteger(channel.idx) || channel.idx < 0 || channel.idx > 7) {
    throw new Error('switch idx must be an integer in 0..7')
  }
  return enqueue(deviceKey, 'set_switch', { ...channel, ...rule })
}

/**
 * Energize or de-energize a relay. Thin wrapper around set_switch that
 * sends only the relay-state fields. Used by ChannelsPage and relayService
 * for manual override without disturbing the rule configuration.
 */
export async function setRelayEnergized(
  deviceKey: string,
  idx: number,
  is_energized: boolean,
  active_high: boolean = true,
) {
  if (!Number.isInteger(idx) || idx < 0 || idx > 7) {
    throw new Error('relay idx must be an integer in 0..7')
  }
  return enqueue(deviceKey, 'set_relay', { idx, is_energized, active_high, enabled: true })
}

// ── Battery ──────────────────────────────────────────────────────────────────

// Matches BatteryChemistryEnum in firmware/include/battery_profile.h:
//   BAT_CHEM_LEAD_ACID = 0, LIION, LFP, LIPO, NICD, NIMH, CUSTOM
// This is the SINGLE CANONICAL BatteryChemistry type across the UI. Legacy
// pages import this from here rather than defining their own. lib/types.ts
// re-exports it for older call sites; do not introduce a second alias.
//
// Static invariant: the order and length of this tuple MUST match the
// firmware's BatteryChemistryEnum. If you add a chemistry here, also add
// the matching enum value in firmware/include/battery_profile.h and bump
// BATTERY_NUM_CHEM in firmware.
export type BatteryChemistry =
  | 'lead_acid' | 'liion' | 'lfp' | 'lipo' | 'nicd' | 'nimh' | 'custom'

// Compile-time guard: tuple length must equal 7 (the firmware's
// BatteryChemistryEnum width). The runtime throw only fires if the tuple
// is mutated at runtime via `as any`; the static type-check is the real
// protection.
const _BATTERY_CHEMISTRY_TUPLE: readonly BatteryChemistry[] = [
  'lead_acid', 'liion', 'lfp', 'lipo', 'nicd', 'nimh', 'custom',
] as const
if (_BATTERY_CHEMISTRY_TUPLE.length !== 7) {
  throw new Error('BatteryChemistry tuple length does not match BatteryChemistryEnum')
}

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

/**
 * Bind a logical channel to a battery profile.
 *
 * Binds a channel to a battery profile. Replaces the legacy
 * capacity_mAh-based set_battery. The legacy set_battery (capacity_mAh +
 * initial_soc_pct) is still handled by the device's polled handler as a
 * fallback for older firmware; new code should always use this new shape.
 */
export async function setBattery(deviceKey: string, channel: number, profile_id: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (!Number.isInteger(profile_id) || profile_id < 0 || profile_id > 15) {
    throw new Error('profile_id must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'set_battery', { channel, profile_id })
}

/**
 * Legacy capacity-only battery binding.
 *
 * Some very old firmwares (and the SettingsPage "Save Basic" path) still
 * send a flat `{channel, capacity_mAh, initial_soc_pct}` payload rather than
 * a profile id. The device's polled handler accepts this as a fallback for
 * the new `set_battery` shape; new code should prefer setBattery with a
 * profile_id. Kept for backward compatibility.
 */
export async function setBatteryLegacy(
  deviceKey: string,
  channel: number,
  capacity_mAh: number,
  initial_soc_pct: number,
) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (!Number.isFinite(capacity_mAh) || capacity_mAh < 0) {
    throw new Error('capacity_mAh must be a non-negative finite number')
  }
  if (!Number.isFinite(initial_soc_pct) || initial_soc_pct < 0 || initial_soc_pct > 100) {
    throw new Error('initial_soc_pct must be a finite number in 0..100')
  }
  return enqueue(deviceKey, 'set_battery', { channel, capacity_mAh, initial_soc_pct })
}

/** Create or update a custom battery profile (or overwrite a built-in). */
export async function setBatteryProfile(deviceKey: string, profile: BatteryChemistryProfile) {
  if (!Number.isInteger(profile.id) || profile.id < 0 || profile.id > 15) {
    throw new Error('profile.id must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'set_battery_profile', profile as unknown as Record<string, unknown>)
}

/** Delete a custom battery profile (built-ins cannot be deleted). */
export async function deleteBatteryProfile(deviceKey: string, id: number) {
  if (!Number.isInteger(id) || id < 0 || id > 15) {
    throw new Error('id must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'delete_battery_profile', { id })
}

/** Reset a channel's cycle accumulator + capacity test state. */
export async function resetBattery(deviceKey: string, channel: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'reset_battery', { channel })
}

/**
 * Read the bound battery profile id for a channel.
 *
 * STUB: do not call this. Reads do not flow through settings_commands.
 * The web should read directly from the `battery_bindings` table; the
 * polled-command path does not have a `get_battery` branch and adding one
 * would cost a round-trip per page load. Kept here for API symmetry; the
 * implementation throws if invoked.
 */
export async function getBattery(_deviceKey: string, _channel: number): Promise<never> {
  throw new Error(
    'getBattery is not implemented — read from the battery_bindings table directly.',
  )
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
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (!Number.isInteger(type) || type < 0 || type > 3) {
    throw new Error('type must be 0..3 (volt_offset_mv, volt_gain, curr_offset_ma, curr_gain)')
  }
  if (!Number.isFinite(value)) {
    throw new Error('value must be a finite number')
  }
  return enqueue(deviceKey, 'set_calibration', { channel, type, value })
}

export async function resetCalibration(deviceKey: string, channel: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'reset_calibration', { channel })
}

export async function setInvertCurrent(deviceKey: string, channel: number, invert: boolean) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'set_invert_curr', { channel, invert })
}

export async function setShunt(deviceKey: string, channel: number, ohms: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (!Number.isFinite(ohms) || ohms < 0) {
    throw new Error('ohms must be a non-negative finite number')
  }
  return enqueue(deviceKey, 'set_shunt', { channel, ohms })
}

export async function setVoltRatio(deviceKey: string, channel: number, ratio: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (!Number.isFinite(ratio) || ratio <= 0) {
    throw new Error('ratio must be a positive finite number')
  }
  return enqueue(deviceKey, 'set_volt_ratio', { channel, ratio })
}

export async function setResistors(deviceKey: string, channel: number, r_high: number, r_low: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (!Number.isFinite(r_high) || r_high <= 0) {
    throw new Error('r_high must be a positive finite number')
  }
  if (!Number.isFinite(r_low) || r_low <= 0) {
    throw new Error('r_low must be a positive finite number')
  }
  return enqueue(deviceKey, 'set_resistors', { channel, r_high, r_low })
}

export async function calibrateBaseline(deviceKey: string) {
  return enqueue(deviceKey, 'calibrate_baseline', {})
}

export async function resetCoulomb(deviceKey: string, channel: number) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  return enqueue(deviceKey, 'reset_coulomb', { channel })
}

export async function setChannelName(deviceKey: string, channel: number, name: string) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 15) {
    throw new Error('channel must be an integer in 0..15')
  }
  if (typeof name !== 'string') {
    throw new Error('name must be a string')
  }
  return enqueue(deviceKey, 'set_channel_name', { channel, name })
}

/**
 * Configure a virtual channel (source mapping for voltage/current).
 *
 * `voltage_src` (0..4): 0=none, 1=ina3221_volt, 2=ina3221_curr, 3=ina226,
 * 4=ads1115. `current_src` (0..3): 0=none, 1=ina3221, 2=ina226. See
 * docs/API.md for the full `set_virtual_channel` schema.
 */
export async function setVirtualChannel(
  deviceKey: string,
  channel: number,
  config: { voltage_src: number; voltage_idx: number; current_src: number; current_idx: number },
) {
  if (!Number.isInteger(channel) || channel < 0 || channel > 3) {
    throw new Error('channel must be an integer in 0..3')
  }
  if (!Number.isInteger(config.voltage_src) || config.voltage_src < 0 || config.voltage_src > 4) {
    throw new Error('voltage_src must be an integer in 0..4')
  }
  if (!Number.isInteger(config.voltage_idx) || config.voltage_idx < 0 || config.voltage_idx > 15) {
    throw new Error('voltage_idx must be a non-negative integer in 0..15')
  }
  if (!Number.isInteger(config.current_src) || config.current_src < 0 || config.current_src > 3) {
    throw new Error('current_src must be an integer in 0..3')
  }
  if (!Number.isInteger(config.current_idx) || config.current_idx < 0 || config.current_idx > 15) {
    throw new Error('current_idx must be a non-negative integer in 0..15')
  }
  return enqueue(deviceKey, 'set_virtual_channel', { channel, ...config })
}

// ── Connectivity / provisioning ──────────────────────────────────────────────

/**
 * Configure the device's WiFi. The SSID is required; an empty string is
 * rejected so the device does not blank out an existing credential.
 */
export async function setWifi(deviceKey: string, ssid: string, pass: string) {
  if (typeof ssid !== 'string' || ssid.length === 0) {
    throw new Error('ssid is required and must be a non-empty string')
  }
  if (typeof pass !== 'string') {
    throw new Error('pass must be a string (empty string is allowed)')
  }
  return enqueue(deviceKey, 'set_wifi', { ssid, pass })
}

/**
 * Configure the device's MQTT broker. The firmware polled handler currently
 * does not save `user` / `pass`; this wrapper accepts broker/port/topic only
 * to match the audited surface. MQTT auth is a future firmware addition.
 */
export async function setMqtt(deviceKey: string, broker: string, port: number, topic: string) {
  if (typeof broker !== 'string' || broker.length === 0) {
    throw new Error('broker is required and must be a non-empty string')
  }
  if (!Number.isInteger(port) || port < 1 || port > 65535) {
    throw new Error('port must be an integer in 1..65535')
  }
  if (typeof topic !== 'string') {
    throw new Error('topic must be a string')
  }
  return enqueue(deviceKey, 'set_mqtt', { broker, port, topic })
}

/**
 * Configure the optional HTTP publish endpoint. The `enabled` flag controls
 * whether the device POSTs to this URL on each sample. The audited set_http
 * was previously dropping `enabled`; it is now included in the payload.
 */
export async function setHttp(deviceKey: string, url: string, token?: string, enabled: boolean = true) {
  if (typeof url !== 'string') {
    throw new Error('url must be a string')
  }
  if (typeof enabled !== 'boolean') {
    throw new Error('enabled must be a boolean')
  }
  return enqueue(deviceKey, 'set_http', { url, token: token ?? '', enabled })
}

/**
 * Configure the device's Supabase connection. The firmware polled handler
 * uses non-empty `api_key` and `device_key` to overwrite the existing
 * values; empty strings are treated as "do not change" so partial updates
 * from the legacy form don't wipe credentials. The default of '' here
 * matches the firmware's defensive behavior.
 */
export async function setSupabase(
  deviceKey: string,
  url: string,
  anon_key: string,
  api_key?: string,
  device_key_arg?: string,
) {
  if (typeof url !== 'string' || url.length === 0) {
    throw new Error('url is required and must be a non-empty string')
  }
  if (typeof anon_key !== 'string' || anon_key.length === 0) {
    throw new Error('anon_key is required and must be a non-empty string')
  }
  return enqueue(deviceKey, 'set_supabase', {
    url,
    anon_key,
    api_key: api_key ?? '',
    device_key: device_key_arg ?? '',
  })
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
