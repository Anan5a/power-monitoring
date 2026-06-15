interface SemiGaugeProps {
  value: number
  max: number
  color?: string
  size?: number
  stroke?: number
}

export default function SemiGauge({
  value,
  max,
  color = '#f97316',
  size = 80,
  stroke = 10,
}: SemiGaugeProps) {
  const r = (size - stroke * 2) / 2
  const cx = size / 2
  const cy = size - stroke
  const pct = Math.min(value, max) / max

  // Build a semi-circle arc path from bottom-left to bottom-right
  const arcPath = [
    `M ${cx - r} ${cy}`,
    `A ${r} ${r} 0 0 1 ${cx + r} ${cy}`,
  ].join(' ')

  // Arc length = π * r (half circumference)
  const arcLen = Math.PI * r
  const offset = arcLen * (1 - pct)

  return (
    <svg width={size} height={size / 2 + stroke} viewBox={`0 0 ${size} ${size / 2 + stroke}`}>
      {/* Background track */}
      <path
        d={arcPath}
        fill="none"
        stroke="#e5e7eb"
        strokeWidth={stroke}
        strokeLinecap="round"
      />
      {/* Filled arc */}
      <path
        d={arcPath}
        fill="none"
        stroke={color}
        strokeWidth={stroke}
        strokeDasharray={arcLen}
        strokeDashoffset={offset}
        strokeLinecap="round"
        className="transition-all duration-700"
      />
    </svg>
  )
}
