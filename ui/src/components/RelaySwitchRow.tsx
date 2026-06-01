import { useEffect, useState, useRef } from 'react'
import { supabase } from '../lib/supabase'
import ToggleSwitch from './ui/ToggleSwitch'
import type { RelayState } from '../lib/types'

interface Props {
  deviceKey: string
}

export default function RelaySwitchRow({ deviceKey }: Props) {
  const [relays, setRelays] = useState<RelayState[]>([])
  const [busy, setBusy] = useState<Record<number, boolean>>({})
  const channelRef = useRef<ReturnType<typeof supabase.channel> | null>(null)

  useEffect(() => {
    if (!deviceKey) return

    let cancelled = false
    async function load() {
      const { data } = await supabase
        .from('relay_states')
        .select('*')
        .eq('device_key', deviceKey)
        .order('relay_index')
      if (!cancelled && data) setRelays(data as RelayState[])
    }
    load()

    const channel = supabase
      .channel(`relay-state-row-${deviceKey}`)
      .on('postgres_changes', {
        event: '*',
        schema: 'public',
        table: 'relay_states',
        filter: `device_key=eq.${deviceKey}`,
      }, payload => {
        const r = payload.new as RelayState
        setRelays(prev => {
          const idx = prev.findIndex(rel => rel.id === r.id)
          if (idx >= 0) {
            const next = [...prev]
            next[idx] = r
            return next
          }
          return [...prev, r].sort((a, b) => a.relay_index - b.relay_index)
        })
      })
      .subscribe()
    channelRef.current = channel

    return () => {
      cancelled = true
      if (channelRef.current) {
        supabase.removeChannel(channelRef.current)
        channelRef.current = null
      }
    }
  }, [deviceKey])

  async function toggle(relay: RelayState) {
    const newState = !relay.is_energized
    setBusy(b => ({ ...b, [relay.id]: true }))
    const { error } = await supabase.from('settings_commands').insert({
      device_key: deviceKey,
      cmd_type: 'set_relay',
      payload: {
        idx: relay.relay_index,
        is_energized: newState,
        active_high: relay.active_high ?? true,
        enabled: true,
        overcurrent_A: 0,
        undervoltage_V: 0,
        soc_low_pct: 0,
        soc_high_pct: 100,
        trip_delay_ms: 500,
        reset_delay_ms: 5000,
      },
      status: 'pending',
    })
    if (error) {
      setBusy(b => ({ ...b, [relay.id]: false }))
    }
    // Optimistic update — let the realtime channel reconcile later
    setRelays(prev => prev.map(r => r.id === relay.id ? { ...r, is_energized: newState } : r))
    // Clear busy after a short window
    setTimeout(() => setBusy(b => ({ ...b, [relay.id]: false })), 1500)
  }

  if (relays.length === 0) {
    return null
  }

  return (
    <div className="bg-white rounded-2xl shadow-[0_1px_2px_rgba(15,23,42,0.04),0_1px_3px_rgba(15,23,42,0.06)] border border-slate-100 p-4">
      <div className="flex items-center justify-between mb-3">
        <h3 className="font-semibold text-slate-800">Relays</h3>
        <span className="text-xs text-slate-400">{relays.length} switch{relays.length === 1 ? '' : 'es'}</span>
      </div>
      <div className="flex gap-3 overflow-x-auto pb-1">
        {relays.map(relay => (
          <div
            key={relay.id}
            className="flex-shrink-0 min-w-[110px] flex flex-col items-center gap-1.5 px-3 py-2.5 rounded-xl bg-slate-50 border border-slate-100"
          >
            <div className="flex items-center justify-between w-full">
              <span className="text-xs font-semibold text-slate-700">R{relay.relay_index}</span>
              <span
                className={`text-[10px] font-semibold uppercase tracking-wider ${
                  relay.is_energized ? 'text-emerald-600' : 'text-slate-400'
                }`}
              >
                {relay.is_energized ? 'On' : 'Off'}
              </span>
            </div>
            <ToggleSwitch
              checked={relay.is_energized}
              onChange={() => toggle(relay)}
              disabled={busy[relay.id]}
              size="md"
              label={undefined}
            />
            <span className="text-[10px] text-slate-400 font-mono">
              GPIO {relay.gpio_pin}
            </span>
          </div>
        ))}
      </div>
    </div>
  )
}
