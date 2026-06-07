import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts'
import type { TelemetryPoint } from '../lib/types'

interface Props {
  data: TelemetryPoint[]
  dataKey: string
  color?: string
}

export default function TelemetryChart({ data, dataKey, color = 'blue' }: Props) {
  const chartData = data.map(point => ({
    time: new Date(point.recorded_at).toLocaleTimeString(),
    value: point.payload?.[dataKey] ?? 0,
  }))

  return (
    <ResponsiveContainer width="100%" height={200}>
      <LineChart data={chartData}>
        <CartesianGrid strokeDasharray="3 3" stroke="#f0f0f0" />
        <XAxis
          dataKey="time"
          tick={{ fontSize: 10 }}
          interval="preserveStartEnd"
        />
        <YAxis tick={{ fontSize: 10 }} width={40} />
        <Tooltip
          formatter={(value) => [typeof value === 'number' ? value.toFixed(4) : value, dataKey]}
          labelFormatter={(label) => `Time: ${label}`}
        />
        <Line
          type="monotone"
          dataKey="value"
          stroke={color}
          strokeWidth={2}
          dot={false}
          isAnimationActive={false}
        />
      </LineChart>
    </ResponsiveContainer>
  )
}