import { memo, useEffect, useState } from 'react'
import { useAtomValue, useStore } from 'jotai'
import { relayStatesAtomFamily } from '../state/atoms'
import { loadRelays, subscribeRelays, toggleRelay } from '../state/services/relayService'
import ToggleSwitch from '../components/ui/ToggleSwitch'

interface Props {
  deviceKey: string
}

function RelaysWidget({ deviceKey }: Props) {
  const relays = useAtomValue(relayStatesAtomFamily(deviceKey))
  const store = useStore()
  const [busy, setBusy] = useState<Record<number, boolean>>({})

  useEffect(() => {
    let cleanup: (() => void) | undefined
    loadRelays(store, deviceKey).then(() => {
      cleanup = subscribeRelays(store, deviceKey)
    })
    return () => { if (cleanup) cleanup() }
  }, [deviceKey, store])

  if (relays.length === 0) {
    return (
      <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-5 flex items-center text-slate-300 text-sm">
        No relays configured
      </div>
    )
  }

  return (
    <div className="h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 px-5 py-3 overflow-x-auto overflow-y-hidden">
      <div className="flex items-center justify-between mb-2">
        <h3 className="font-semibold text-slate-800 text-sm">Relays</h3>
        <span className="text-xs text-slate-400">{relays.length} switch{relays.length === 1 ? '' : 'es'}</span>
      </div>
      <div className="flex gap-3">
        {relays.map(relay => (
          <div key={relay.id} className="flex-shrink-0 min-w-[110px] flex flex-col items-center gap-1 px-3 py-2 rounded-xl bg-slate-50 border border-slate-100">
            <div className="flex items-center justify-between w-full">
              <span className="text-xs font-semibold text-slate-700">R{relay.relay_index}</span>
              <span className={`text-[10px] font-semibold uppercase tracking-wider transition-colors duration-200 ${relay.is_energized ? 'text-emerald-600' : 'text-slate-400'}`}>
                {relay.is_energized ? 'On' : 'Off'}
              </span>
            </div>
            <ToggleSwitch
              checked={relay.is_energized}
              onChange={() => {
                setBusy(b => ({ ...b, [relay.id]: true }))
                toggleRelay(store, deviceKey, relay, !relay.is_energized).finally(() => {
                  setTimeout(() => setBusy(b => ({ ...b, [relay.id]: false })), 1500)
                })
              }}
              disabled={busy[relay.id]}
              size="md"
              label={undefined}
            />
            <span className="text-[10px] text-slate-400 font-mono">GPIO {relay.gpio_pin}</span>
          </div>
        ))}
      </div>
    </div>
  )
}

export default memo(RelaysWidget)