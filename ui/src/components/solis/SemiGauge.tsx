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
  const r = (size - stroke) / 2
  const cx = size / 2
  const cy = size / 2
  const arc = Math.PI * r
  const offset = arc - (Math.min(value, max) / max) * arc

  return (
    <svg width={size} height={size / 2 + 4}>
      <path
        d={`M ${cx - r} ${cy} A ${r} ${r} 0 0 1 ${cx + r} ${cy}`}
        fill="none"
        stroke="#e5e7eb"
        strokeWidth={stroke}
        strokeLinecap="round"
      />
      <path
        d={`M ${cx - r} ${cy} A ${r} ${r} 0 0 1 ${cx + r} ${cy}`}
        fill="none"
        stroke={color}
        strokeWidth={stroke}
        strokeDasharray={arc}
        strokeDashoffset={offset}
        strokeLinecap="round"
        className="transition-all duration-700"
      />
    </svg>
  )
}
