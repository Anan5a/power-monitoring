interface StatusBadgeProps {
  status: 'normal' | 'warning' | 'offline' | string
  label?: string
}

const colorMap: Record<string, string> = {
  normal: 'bg-green-500',
  warning: 'bg-amber-500',
  offline: 'bg-gray-400',
}

export default function StatusBadge({ status, label }: StatusBadgeProps) {
  return (
    <span className="inline-flex items-center gap-1.5 text-sm text-gray-700">
      <span className={`h-2 w-2 rounded-full ${colorMap[status] ?? colorMap.normal}`} />
      {label ?? status}
    </span>
  )
}
