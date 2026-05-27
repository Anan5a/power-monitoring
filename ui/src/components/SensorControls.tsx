import { useState, useEffect } from 'react'
import { supabase } from '../lib/supabase'
import type { DeviceChannels } from '../lib/types'

interface CalStatus {
  calibrating: boolean
  baseline_tick: number
  baseline_stddev: Record<string, number>
}

interface Props {
  deviceKey: string | null
  deviceChannels: DeviceChannels | null
}

export default function SensorControls({ deviceKey, deviceChannels }: Props) {
  const [calStatus, setCalStatus] = useState<CalStatus | null>(null)
  const [showDone, setShowDone] = useState(false)
  const [doneTimer, setDoneTimer] = useState<ReturnType<typeof setTimeout> | null>(null)

  // Poll calibration status every 1s when device is selected
  useEffect(() => {
    if (!deviceKey) return
    const fetch = async () => {
      const { data } = await supabase
        .from('sensor_calibration_status')
        .select('*')
        .eq('device_key', deviceKey)
        .maybeSingle()
      if (data) {
        setCalStatus(data as CalStatus)
      }
    }
    fetch()
    const id = setInterval(fetch, 1000)
    return () => clearInterval(id)
  }, [deviceKey])

  // Show "Done ✓" for 3s after calibrating transitions to false
  useEffect(() => {
    if (calStatus && !calStatus.calibrating && calStatus.baseline_tick >= 10) {
      setShowDone(true)
      if (doneTimer) clearTimeout(doneTimer)
      const t = setTimeout(() => setShowDone(false), 3000)
      setDoneTimer(t)
    }
  }, [calStatus])

  const runBaselineCal = async () => {
    if (!deviceKey) return
    const { error } = await supabase.from('settings_commands').insert({
      device_key: deviceKey,
      cmd_type: 'calibrate_baseline',
      payload: {},
      status: 'pending',
    })
    if (error) console.error('Failed to send calibrate_baseline:', error)
  }

  const setCalibration = async (channel: number, type: number, value: number) => {
    if (!deviceKey) return
    const { error } = await supabase.from('settings_commands').insert({
      device_key: deviceKey,
      cmd_type: 'set_calibration',
      payload: { channel, type, value },
      status: 'pending',
    })
    if (error) console.error('Failed to set calibration:', error)
  }

  // Load current calibration from deviceChannels or use defaults
  const currentCal = deviceChannels?.channel_calibration

  return (
    <div className="bg-white rounded-lg shadow p-4 mb-6">
      <div className="flex items-center justify-between mb-3">
        <h3 className="font-semibold text-gray-700">Sensor Calibration</h3>
        {calStatus && calStatus.calibrating && (
          <span className="px-3 py-1 rounded-full text-xs font-medium bg-blue-100 text-blue-700">
            Baseline Cal... {calStatus.baseline_tick}/10
          </span>
        )}
        {showDone && (
          <span className="px-3 py-1 rounded-full text-xs font-medium bg-green-100 text-green-700">
            Done ✓
          </span>
        )}
      </div>

      {/* Baseline calibration button */}
      <div className="mb-4">
        <div className="flex items-center gap-3">
          <button
            onClick={runBaselineCal}
            disabled={!deviceKey || (calStatus?.calibrating ?? false)}
            className="px-4 py-2 rounded font-medium text-sm bg-blue-600 text-white hover:bg-blue-700 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            Run Baseline Calibration
          </button>
          {calStatus && calStatus.baseline_tick > 0 && !calStatus.calibrating && !showDone && (
            <span className="text-xs text-gray-400">
              baseline_tick={calStatus.baseline_tick} · idle
            </span>
          )}
        </div>

        {/* Progress bar while calibrating */}
        {calStatus && calStatus.calibrating && (
          <div className="mt-2 w-full bg-gray-200 rounded-full h-2">
            <div
              className="h-2 rounded-full bg-blue-500 transition-all"
              style={{ width: `${Math.min(calStatus.baseline_tick * 10, 100)}%` }}
            />
          </div>
        )}
      </div>

      {/* Per-channel offset inputs */}
      <div className="border-t pt-3">
        <div className="text-xs text-gray-500 mb-2">Manual offset (zeroes sensor reading when nothing connected)</div>
        <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
          {[0, 1, 2].map(ch => (
            <div key={ch} className="bg-gray-50 rounded p-3">
              <div className="text-xs font-medium text-gray-600 mb-2">CH{ch}</div>
              <div className="space-y-1.5">
                <div className="flex items-center gap-1">
                  <label className="text-xs text-gray-400 w-8">V Off</label>
                  <input
                    type="number"
                    step="0.1"
                    defaultValue={currentCal?.volt_offset_mv?.[ch] ?? 0}
                    className="flex-1 border border-gray-300 rounded px-2 py-0.5 text-xs w-20"
                    id={`ch${ch}-vo`}
                  />
                  <span className="text-xs text-gray-400">mV</span>
                  <button
                    onClick={() => {
                      const v = parseFloat((document.getElementById(`ch${ch}-vo`) as HTMLInputElement).value)
                      if (!isNaN(v)) setCalibration(ch, 0, v)
                    }}
                    className="text-xs px-2 py-0.5 bg-blue-600 text-white rounded hover:bg-blue-700"
                  >
                    Set
                  </button>
                </div>
                <div className="flex items-center gap-1">
                  <label className="text-xs text-gray-400 w-8">I Off</label>
                  <input
                    type="number"
                    step="0.001"
                    defaultValue={currentCal?.curr_offset_ma?.[ch] ?? 0}
                    className="flex-1 border border-gray-300 rounded px-2 py-0.5 text-xs w-20"
                    id={`ch${ch}-co`}
                  />
                  <span className="text-xs text-gray-400">mA</span>
                  <button
                    onClick={() => {
                      const v = parseFloat((document.getElementById(`ch${ch}-co`) as HTMLInputElement).value)
                      if (!isNaN(v)) setCalibration(ch, 2, v)
                    }}
                    className="text-xs px-2 py-0.5 bg-blue-600 text-white rounded hover:bg-blue-700"
                  >
                    Set
                  </button>
                </div>
              </div>
            </div>
          ))}
        </div>
        <div className="text-xs text-gray-400 mt-2">
          type 0=volt_offset_mv, 1=volt_gain, 2=curr_offset_ma, 3=curr_gain
        </div>
      </div>
    </div>
  )
}