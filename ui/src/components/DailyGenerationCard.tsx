import { motion, AnimatePresence } from 'framer-motion'
import { SunIcon } from '@heroicons/react/24/outline'
import { AreaChart, Area, ResponsiveContainer, Tooltip } from 'recharts'
import { useDailyGeneration } from '../hooks/useDailyGeneration'

interface Props {
  deviceKey: string
}

export default function DailyGenerationCard({ deviceKey }: Props) {
  const { total, hourly, isLoading } = useDailyGeneration(deviceKey)

  const chartData = hourly.map(h => ({
    time: h.hour,
    kWh: h.value,
  }))

  return (
    <div className="bg-gradient-to-br from-amber-50/50 to-white bg-white rounded-2xl shadow-sm border border-slate-100 p-6">
      <div className="flex items-start justify-between mb-3">
        <div className="flex items-center gap-2">
          <SunIcon className="w-5 h-5 text-amber-400" />
          <span className="text-[11px] uppercase tracking-wider text-slate-400 font-semibold">Today's Generation</span>
        </div>
        <span className="text-xs text-amber-600 font-medium">kWh</span>
      </div>

      <AnimatePresence mode="popLayout" initial={false}>
        <motion.div
          key={total}
          initial={{ y: 8, opacity: 0 }}
          animate={{ y: 0, opacity: 1 }}
          exit={{ y: -8, opacity: 0 }}
          transition={{ duration: 0.3 }}
          className="flex items-baseline gap-1.5 mb-4"
        >
          {isLoading ? (
            <div className="h-9 w-24 bg-slate-100 rounded animate-pulse" />
          ) : (
            <>
              <span className="text-4xl font-bold text-amber-600 tabular-nums">
                {total > 0 ? total.toFixed(2) : '0.00'}
              </span>
              <span className="text-lg font-medium text-amber-500">kWh</span>
            </>
          )}
        </motion.div>
      </AnimatePresence>

      {/* Sparkline */}
      {chartData.length > 0 && (
        <div className="h-16">
          <ResponsiveContainer width="100%" height="100%">
            <AreaChart data={chartData} margin={{ top: 2, right: 0, left: 0, bottom: 0 }}>
              <defs>
                <linearGradient id="gradGen" x1="0" y1="0" x2="0" y2="1">
                  <stop offset="5%" stopColor="#f59e0b" stopOpacity={0.4} />
                  <stop offset="95%" stopColor="#f59e0b" stopOpacity={0.05} />
                </linearGradient>
              </defs>
              <Tooltip
                formatter={(v: number) => [`${v.toFixed(3)} kWh`, 'Generation']}
                labelFormatter={(l: string) => l}
                contentStyle={{ fontSize: 11, padding: '2px 6px' }}
              />
              <Area
                type="monotone"
                dataKey="kWh"
                stroke="#f59e0b"
                strokeWidth={1.5}
                fill="url(#gradGen)"
                dot={false}
                connectNulls
              />
            </AreaChart>
          </ResponsiveContainer>
        </div>
      )}

      {chartData.length > 0 && (
        <div className="text-[10px] text-slate-400 mt-1 text-right">
          00:00 → {chartData[chartData.length - 1]?.time ?? 'now'}
        </div>
      )}
    </div>
  )
}