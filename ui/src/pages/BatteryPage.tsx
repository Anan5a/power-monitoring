// BatteryPage.tsx
// =============================================================================
// Web surface for battery profiles, cycle count, and capacity test.
//
// Mirrors the BLE surface in docs/API.md — every command is sent via
// lib/deviceCommands.ts. Reads come from Supabase tables that the firmware
// populates:
//   - battery_profiles: full profile list, updated every 60s by the device
//   - telemetry_live: latest battery[] array with soc_pct, cycles, V, I
//   - capacity_test_status: per-channel test state
//
// This page is the *skeleton* — forms are functional but minimal styling.
// Polish can come in a follow-up.
// =============================================================================

import { useState, useEffect } from 'react'
import { supabase } from '../lib/supabase'
import {
  setBattery,
  setBatteryProfile,
  deleteBatteryProfile,
  resetBattery,
  capacityTestStart,
  capacityTestStop,
  capacityTestStatus,
  type BatteryChemistryProfile,
  type BatteryChemistry,
  type CapacityTestStartParams,
} from '../lib/deviceCommands'

interface Props {
  deviceKey: string | null
  channelCount: number
}

const CHEMISTRIES: BatteryChemistry[] = ['lead_acid', 'liion', 'lfp', 'lipo', 'nicd', 'nimh', 'custom']

export default function BatteryPage({ deviceKey, channelCount }: Props) {
  const [profiles, setProfiles] = useState<BatteryChemistryProfile[]>([])
  const [bindings, setBindings] = useState<Record<number, number>>({})
  // Pending bindings: keys are channel numbers whose binding change has been
  // queued to the device but not yet confirmed by the next heartbeat. The
  // 60s heartbeat will eventually overwrite the local bindings from
  // battery_bindings; until then we display a "pending" badge so the user
  // knows the device hasn't applied the change yet.
  const [pendingBindings, setPendingBindings] = useState<Set<number>>(new Set())
  const [error, setError] = useState<string | null>(null)
  const [testStatus, setTestStatus] = useState<Record<number, string>>({})

  useEffect(() => {
    if (!deviceKey) return
    const load = async () => {
      const [{ data: prof }, { data: bind }] = await Promise.all([
        supabase.from('battery_profiles').select('*').eq('device_key', deviceKey).order('id'),
        supabase.from('battery_bindings').select('*').eq('device_key', deviceKey),
      ])
      if (prof) setProfiles(prof as BatteryChemistryProfile[])
      if (bind) {
        const m: Record<number, number> = {}
        for (const b of bind as Array<{ channel: number; profile_id: number }>) m[b.channel] = b.profile_id
        setBindings(m)
        // Heartbeat arrived: clear any pending markers. We do this even if
        // the value didn't change, because the firmware applied (or ignored)
        // the queued command by now.
        setPendingBindings(new Set())
      }
    }
    load()
    const id = setInterval(load, 65000)  // battery_profiles heartbeat is 60s
    return () => clearInterval(id)
  }, [deviceKey])

  if (!deviceKey) {
    return <div className="p-4 text-gray-500">Select a device to manage batteries.</div>
  }

  const bindChannel = async (channel: number, profileId: number) => {
    setError(null)
    // Mark the binding as pending BEFORE the network call so the badge
    // appears instantly. We do not update the local bindings map yet —
    // the value is only committed when the next heartbeat confirms it.
    // This avoids a brief flash of the wrong value if the device rejects
    // the queued command.
    setPendingBindings(p => new Set(p).add(channel))
    try {
      await setBattery(deviceKey, channel, profileId)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'bind failed')
      // Drop the pending marker on hard failure so the UI doesn't stay
      // stuck. The local bindings map was never updated, so it still
      // shows the last-confirmed value.
      setPendingBindings(p => {
        const next = new Set(p)
        next.delete(channel)
        return next
      })
    }
  }

  const removeProfile = async (id: number) => {
    if (id < 4) {
      setError('Built-in profiles cannot be deleted. Use set_battery_profile to overwrite.')
      return
    }
    if (!confirm(`Delete profile ${id}?`)) return
    try {
      await deleteBatteryProfile(deviceKey, id)
      setProfiles(ps => ps.filter(p => p.id !== id))
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'delete failed')
    }
  }

  const resetChannel = async (channel: number) => {
    if (!confirm(`Reset cycle accumulator and capacity test state for channel ${channel}?`)) return
    try {
      await resetBattery(deviceKey, channel)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'reset failed')
    }
  }

  const startTest = async (channel: number) => {
    const mode = prompt('Mode (manual or automated):', 'manual') as 'manual' | 'automated' | null
    if (!mode) return
    const params: CapacityTestStartParams = { channel, mode }
    if (mode === 'automated') {
      const ls = prompt('Load switch idx (0..7):')
      const cv = prompt('Cutoff voltage (V):')
      if (!ls || !cv) return
      params.load_switch_idx = parseInt(ls, 10)
      params.cutoff_v = parseFloat(cv)
    }
    try {
      await capacityTestStart(deviceKey, params)
      setTestStatus(s => ({ ...s, [channel]: 'started' }))
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'start failed')
    }
  }

  const stopTest = async (channel: number) => {
    try {
      await capacityTestStop(deviceKey, channel)
      setTestStatus(s => ({ ...s, [channel]: 'stopped' }))
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'stop failed')
    }
  }

  const fetchStatus = async (channel: number) => {
    try {
      await capacityTestStatus(deviceKey, channel)
      setTestStatus(s => ({ ...s, [channel]: 'requested' }))
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'status failed')
    }
  }

  return (
    <div className="p-4 space-y-6">
      {error && (
        <div className="rounded bg-red-50 border border-red-200 text-red-800 p-3 text-sm">{error}</div>
      )}

      <section>
        <h2 className="text-lg font-semibold mb-3">Channel bindings</h2>
        <div className="grid grid-cols-1 sm:grid-cols-2 md:grid-cols-3 gap-3">
          {Array.from({ length: channelCount }).map((_, ch) => {
            const isPending = pendingBindings.has(ch)
            return (
              <div key={ch} className="border rounded p-3 bg-white">
                <div className="text-sm font-medium mb-2 flex items-center gap-2">
                  <span>Channel {ch}</span>
                  {isPending && (
                    <span
                      title="Queued to device — will appear in the dropdown after the next 60s heartbeat."
                      className="text-[10px] font-medium uppercase tracking-wide px-1.5 py-0.5 rounded bg-amber-100 text-amber-800"
                    >
                      Pending device sync
                    </span>
                  )}
                </div>
                <select
                  className="border rounded px-2 py-1 w-full text-sm"
                  value={bindings[ch] ?? ''}
                  onChange={e => bindChannel(ch, parseInt(e.target.value, 10))}
                >
                  <option value="">— no battery —</option>
                  {profiles.map(p => (
                    <option key={p.id} value={p.id}>{p.name} ({p.chemistry})</option>
                  ))}
                </select>
                <div className="mt-2 flex gap-2 text-xs">
                  <button onClick={() => resetChannel(ch)} className="text-red-600 hover:underline">Reset cycle</button>
                  <button onClick={() => startTest(ch)} className="text-blue-600 hover:underline">Cap test</button>
                  <button onClick={() => stopTest(ch)} className="text-gray-600 hover:underline">Stop</button>
                  <button onClick={() => fetchStatus(ch)} className="text-gray-600 hover:underline">Status</button>
                </div>
                {testStatus[ch] && <div className="mt-1 text-xs text-gray-500">test: {testStatus[ch]}</div>}
              </div>
            )
          })}
        </div>
      </section>

      <section>
        <h2 className="text-lg font-semibold mb-3">Battery profiles</h2>
        <table className="w-full text-sm border-collapse">
          <thead>
            <tr className="text-left bg-gray-50">
              <th className="p-2 border-b">ID</th>
              <th className="p-2 border-b">Name</th>
              <th className="p-2 border-b">Chemistry</th>
              <th className="p-2 border-b">Nominal V</th>
              <th className="p-2 border-b">Capacity (Ah)</th>
              <th className="p-2 border-b">Cutoff V</th>
              <th className="p-2 border-b">Float V</th>
              <th className="p-2 border-b">C-rating</th>
              <th className="p-2 border-b"></th>
            </tr>
          </thead>
          <tbody>
            {profiles.map(p => (
              <tr key={p.id} className="hover:bg-gray-50">
                <td className="p-2 border-b font-mono text-xs">{p.id}</td>
                <td className="p-2 border-b">{p.name}</td>
                <td className="p-2 border-b">{p.chemistry}</td>
                <td className="p-2 border-b">{p.nominal_voltage?.toFixed(2) ?? '—'}</td>
                <td className="p-2 border-b">{p.rated_capacity_Ah?.toFixed(2) ?? '—'}</td>
                <td className="p-2 border-b">{p.cutoff_voltage?.toFixed(2) ?? '—'}</td>
                <td className="p-2 border-b">{p.float_voltage?.toFixed(2) ?? '—'}</td>
                <td className="p-2 border-b">{p.c_rating?.toFixed(2) ?? '—'}</td>
                <td className="p-2 border-b text-right">
                  <button
                    onClick={() => removeProfile(p.id)}
                    className="text-xs text-red-600 hover:underline"
                  >
                    Delete
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <NewProfileForm deviceKey={deviceKey} onCreated={p => setProfiles(ps => [...ps, p])} />
      </section>
    </div>
  )
}

function NewProfileForm({ deviceKey, onCreated }: { deviceKey: string; onCreated: (p: BatteryChemistryProfile) => void }) {
  const [open, setOpen] = useState(false)
  const [name, setName] = useState('Custom-LFP-100')
  const [chemistry, setChemistry] = useState<BatteryChemistry>('lfp')
  const [capacity, setCapacity] = useState('100')
  const [nominal, setNominal] = useState('3.2')
  const [cutoff, setCutoff] = useState('2.5')
  const [float, setFloat] = useState('3.65')
  const [cRating, setCRating] = useState('1.0')
  const [error, setError] = useState<string | null>(null)

  const submit = async () => {
    setError(null)
    const idStr = prompt('Profile id (0..15, must be ≥ 4 to be deletable as a custom):')
    if (!idStr) return
    const id = parseInt(idStr, 10)
    if (isNaN(id) || id < 0 || id > 15) {
      setError('id must be 0..15')
      return
    }
    const profile: BatteryChemistryProfile = {
      id,
      name,
      chemistry,
      nominal_voltage: parseFloat(nominal),
      rated_capacity_Ah: parseFloat(capacity),
      c_rating: parseFloat(cRating),
      cutoff_voltage: parseFloat(cutoff),
      float_voltage: parseFloat(float),
      charge_efficiency: 0.95,
      cycle_life_rated: 2000,
      min_soc_pct: 10,
      max_soc_pct: 100,
    }
    try {
      await setBatteryProfile(deviceKey, profile)
      onCreated(profile)
      setOpen(false)
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : 'create failed')
    }
  }

  if (!open) {
    return (
      <button onClick={() => setOpen(true)} className="mt-3 px-3 py-1.5 text-sm rounded bg-blue-600 text-white hover:bg-blue-700">
        + New profile
      </button>
    )
  }

  return (
    <div className="mt-3 border rounded p-3 bg-gray-50 space-y-2">
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-2 text-sm">
        <label>Name<input className="border rounded px-2 py-1 w-full" value={name} onChange={e => setName(e.target.value)} /></label>
        <label>Chemistry
          <select className="border rounded px-2 py-1 w-full" value={chemistry} onChange={e => setChemistry(e.target.value as BatteryChemistry)}>
            {CHEMISTRIES.map(c => <option key={c} value={c}>{c}</option>)}
          </select>
        </label>
        <label>Capacity (Ah)<input className="border rounded px-2 py-1 w-full" value={capacity} onChange={e => setCapacity(e.target.value)} /></label>
        <label>Nominal V<input className="border rounded px-2 py-1 w-full" value={nominal} onChange={e => setNominal(e.target.value)} /></label>
        <label>Cutoff V<input className="border rounded px-2 py-1 w-full" value={cutoff} onChange={e => setCutoff(e.target.value)} /></label>
        <label>Float V<input className="border rounded px-2 py-1 w-full" value={float} onChange={e => setFloat(e.target.value)} /></label>
        <label>C-rating<input className="border rounded px-2 py-1 w-full" value={cRating} onChange={e => setCRating(e.target.value)} /></label>
      </div>
      {error && <div className="text-red-600 text-sm">{error}</div>}
      <div className="flex gap-2">
        <button onClick={submit} className="px-3 py-1 text-sm rounded bg-blue-600 text-white hover:bg-blue-700">Save</button>
        <button onClick={() => setOpen(false)} className="px-3 py-1 text-sm rounded border hover:bg-gray-100">Cancel</button>
      </div>
    </div>
  )
}
