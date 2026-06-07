import { memo } from 'react'
import { useAtomValue } from 'jotai'
import { channelPayloadAtomFamily } from '../state/derived'
import { deviceChannelsAtomFamily, latestAtom } from '../state/atoms'

interface Props {
  channel: number
}

function VCCardWidget({ channel }: Props) {
  const payload = useAtomValue(channelPayloadAtomFamily(channel))
  const latest = useAtomValue(latestAtom)
  const channels = useAtomValue(deviceChannelsAtomFamily(latest?.device_id ?? ''))
  const name = channels?.channel_names?.find(c => c.channel === channel)?.name ?? `VC${channel}`
  const batteryCapacity = channels?.battery_profiles?.[channel]?.capacity_mAh ?? 0
  const hasBattery = batteryCapacity > 0
  const online = !!latest

  return (
    <div className={`h-full w-full bg-white rounded-2xl shadow-sm border border-slate-100 border-l-4 ${hasBattery && payload.socPct !== null && payload.socPct < 20 ? 'border-l-yellow-400' : 'border-l-emerald-500'} p-4`}>
      <div className="flex items-center justify-between mb-3">
        <div className="flex items-center gap-2">
          <span className="font-semibold text-slate-800 text-sm">{name}</span>
          <span className={`w-2 h-2 rounded-full ${online ? 'bg-emerald-400' : 'bg-slate-300'}`} />
        </div>
      </div>
      <div className="grid grid-cols-3 gap-2 mb-3">
        <div className="text-center">
          <div className="text-[10px] text-slate-400">V</div>
          <div className="text-xl font-bold text-slate-800 tabular-nums">{payload.voltage !== null ? payload.voltage.toFixed(2) : '--'}</div>
        </div>
        <div className="text-center">
          <div className="text-[10px] text-slate-400">A</div>
          <div className="text-xl font-bold text-slate-800 tabular-nums">{payload.current !== null ? payload.current.toFixed(2) : '--'}</div>
        </div>
        <div className="text-center">
          <div className="text-[10px] text-slate-400">W</div>
          <div className="text-xl font-bold text-slate-800 tabular-nums">{payload.power !== null ? payload.power.toFixed(1) : '--'}</div>
        </div>
      </div>
      {hasBattery && (
        <div>
          <div className="flex justify-between text-[10px] text-slate-500 mb-1">
            <span>SoC</span>
            <span>{payload.socPct !== null ? `${payload.socPct.toFixed(0)}%` : '--'}</span>
          </div>
          <div className="w-full bg-slate-100 rounded-full h-2 overflow-hidden">
            {payload.socPct !== null && (
              <div className="h-2 rounded-full bg-gradient-to-r from-emerald-400 to-teal-500 transition-all duration-300" style={{ width: `${Math.min(payload.socPct, 100)}%` }} />
            )}
          </div>
        </div>
      )}
    </div>
  )
}

export default memo(VCCardWidget)