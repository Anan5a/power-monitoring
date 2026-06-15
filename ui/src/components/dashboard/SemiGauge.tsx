import { useId } from 'react'
import GaugeChart from 'react-gauge-chart'

interface SemiGaugeProps {
  value: number
  max: number
  color?: string
  size?: number
  stroke?: number
}

export default function SemiGauge({ value, max, color = '#f97316', size = 80 }: SemiGaugeProps) {
  const id = useId()
  const pct = max > 0 ? Math.min(value, max) / max : 0

  return (
    <div style={{ width: size, height: size / 2 + 8 }}>
      <GaugeChart
        id={`gauge-${id}`}
        percent={pct}
        nrOfLevels={1}
        colors={[color]}
        textColor="#1f2937"
        needleColor={color}
        needleBaseColor={color}
        hideText
        animate
        animateDuration={700}
        arcWidth={0.25}
        cornerRadius={3}
        marginInPercent={0.05}
      />
    </div>
  )
}
