import { describe, it, expect } from 'vitest'
import { extractKeys } from '../HistoryChartWidget'
import type { TelemetryPoint } from '../../lib/types'

function pt(payload: Record<string, number>): TelemetryPoint {
  return { id: 0, device_id: 'k', recorded_at: new Date().toISOString(), payload, metadata: {} }
}

describe('extractKeys (power)', () => {
  it('returns empty when no data', () => {
    expect(extractKeys([], 'power')).toEqual([])
  })

  it('prefers system power keys when present', () => {
    const data = [
      pt({ pv_power: 100, battery_power: 50, ch0_P: 80, ch1_P: 0, ch2_P: 0, ch3_P: 0 }),
    ]
    const keys = extractKeys(data, 'power')
    expect(keys).toContain('pv_power')
    expect(keys).toContain('battery_power')
    expect(keys).not.toContain('ch0_P')
  })

  it('excludes all-zero series', () => {
    const data = [pt({ pv_power: 100, battery_power: 0 })]
    const keys = extractKeys(data, 'power')
    expect(keys).toEqual(['pv_power'])
  })
})