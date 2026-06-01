import type { ReactNode } from 'react'

export type StatusBadgeStatus = 'charging' | 'discharging' | 'balanced' | 'unknown'

const gradientMap: Record<StatusBadgeStatus, string> = {
  charging: 'bg-gradient-to-r from-cyan-500 to-blue-500 text-white',
  discharging: 'bg-gradient-to-r from-red-500 to-orange-500 text-white',
  balanced: 'bg-gradient-to-r from-emerald-500 to-teal-500 text-white',
  unknown: 'bg-slate-200 text-slate-600',
}

const defaultLabelMap: Record<StatusBadgeStatus, string> = {
  charging: 'Charging',
  discharging: 'Discharging',
  balanced: 'Balanced',
  unknown: 'Unknown',
}

export interface StatusBadgeProps {
  status: StatusBadgeStatus
  children?: ReactNode
  className?: string
}

export default function StatusBadge({ status, children, className = '' }: StatusBadgeProps) {
  const gradient = gradientMap[status]
  const label = children ?? defaultLabelMap[status]
  return (
    <span
      className={`inline-flex items-center px-2.5 py-0.5 rounded-full text-xs font-medium ${gradient} ${className}`.trim()}
    >
      {label}
    </span>
  )
}
