import type { ChannelGroup, BatteryProfile } from './types'

export interface ComputedValues {
  pv_power: number
  battery_power: number
  battery_charging_power: number
  battery_discharging_power: number
  dc_load_power: number
  unclassified_power: number
  inverter_power: number
  system_status: 'charging' | 'discharging' | 'balanced' | 'unknown'
  min_soc_pct: number | null
  max_soc_pct: number | null
  total_energy_wh: number
}

export function computeTelemetry(
  payload: Record<string, number>,
  channelGroups: ChannelGroup[],
  batteryProfiles: BatteryProfile[]
): ComputedValues {
  let pvPower = 0
  let dcLoadPower = 0
  let batteryChargingPower = 0
  let batteryDischargingPower = 0
  let unclassifiedPower = 0

  for (const group of channelGroups) {
    for (let ch = 0; ch < 4; ch++) {
      if ((group.channel_mask & (1 << ch)) === 0) continue
      const p = payload[`ch${ch}_P`] ?? 0
      if (group.icon === 0) {
        pvPower += Math.max(0, p)
      } else if (group.icon === 1) {
        if (p < 0) batteryChargingPower += Math.abs(p)
        else batteryDischargingPower += p
      } else if (group.icon === 2) {
        dcLoadPower += Math.max(0, p)
      } else {
        dcLoadPower += Math.max(0, p)
      }
    }
  }

  const battery_power = batteryDischargingPower - batteryChargingPower
  const inverter_power = batteryChargingPower + dcLoadPower - pvPower

  let system_status: ComputedValues['system_status'] = 'unknown'
  if (batteryChargingPower > 5) system_status = 'charging'
  else if (batteryDischargingPower > 5) system_status = 'discharging'
  else if (Math.abs(inverter_power) <= 5) system_status = 'balanced'

  const socValues = batteryProfiles
    .filter(bp => bp.capacity_mAh > 0)
    .map((_bp, i) => payload[`soc_pct${i}`] ?? null)
    .filter((v): v is number => v !== null)
  const min_soc_pct = socValues.length ? Math.min(...socValues) : null
  const max_soc_pct = socValues.length ? Math.max(...socValues) : null

  const total_energy_wh = [0, 1, 2, 3].reduce(
    (sum, i) => sum + (payload[`energy_wh${i}`] ?? 0),
    0
  )

  return {
    pv_power: pvPower,
    battery_power,
    battery_charging_power: batteryChargingPower,
    battery_discharging_power: batteryDischargingPower,
    dc_load_power: dcLoadPower,
    unclassified_power: unclassifiedPower,
    inverter_power,
    system_status,
    min_soc_pct,
    max_soc_pct,
    total_energy_wh,
  }
}
