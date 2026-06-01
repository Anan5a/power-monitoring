import { motion } from 'framer-motion'
import type { ReactNode } from 'react'

export type ToggleSize = 'sm' | 'md'

export interface ToggleSwitchProps {
  checked: boolean
  onChange: (next: boolean) => void
  label?: ReactNode
  disabled?: boolean
  size?: ToggleSize
}

interface SizeConfig {
  track: string
  thumb: string
  translate: number
}

const sizeMap: Record<ToggleSize, SizeConfig> = {
  sm: { track: 'w-8 h-4', thumb: 'w-3 h-3', translate: 16 },
  md: { track: 'w-11 h-6', thumb: 'w-5 h-5', translate: 20 },
}

export default function ToggleSwitch({
  checked,
  onChange,
  label,
  disabled = false,
  size = 'md',
}: ToggleSwitchProps) {
  const { track, thumb, translate } = sizeMap[size]
  const trackColor = checked
    ? 'bg-gradient-to-r from-emerald-400 to-teal-500'
    : 'bg-slate-200'

  function handleClick() {
    if (disabled) return
    onChange(!checked)
  }

  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={handleClick}
      className={`inline-flex items-center ${disabled ? 'opacity-50 cursor-not-allowed' : 'cursor-pointer'}`}
    >
      {label !== undefined && (
        <span className="mr-3 text-sm text-slate-700 select-none">{label}</span>
      )}
      <span
        className={`relative inline-flex items-center rounded-full transition-colors duration-200 ease-in-out ${track} ${trackColor}`}
      >
        <motion.span
          layout
          className={`inline-block ${thumb} bg-white rounded-full shadow`}
          animate={{ x: checked ? translate : 0 }}
          transition={{ type: 'spring', stiffness: 500, damping: 30 }}
          style={{ marginLeft: 2 }}
        />
      </span>
    </button>
  )
}
