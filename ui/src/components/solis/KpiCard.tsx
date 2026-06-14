import type { ReactNode } from 'react'

interface KpiCardProps {
  label: string
  value: string
  unit?: string
  subText?: ReactNode
  icon?: ReactNode
  accent?: string
}

export default function KpiCard({ label, value, unit, subText, icon, accent }: KpiCardProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm flex items-center gap-4">
      <div className="shrink-0">
        {icon ? (
          <div className={`h-14 w-14 rounded-full flex items-center justify-center text-white ${accent ?? 'bg-orange-500'}`}>
            {icon}
          </div>
        ) : (
          <div className={`h-14 w-14 rounded-full ${accent ?? 'bg-orange-100'}`} />
        )}
      </div>
      <div className="min-w-0">
        <div className="text-sm text-gray-500">{label}</div>
        <div className="text-2xl font-bold text-gray-800 truncate">
          {value}
          {unit && <span className="text-base font-normal text-gray-500 ml-1">{unit}</span>}
        </div>
        {subText && <div className="text-xs text-gray-400 mt-0.5">{subText}</div>}
      </div>
    </div>
  )
}
