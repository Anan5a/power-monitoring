import type { HTMLAttributes, ReactNode } from 'react'

export type CardPadding = 'none' | 'sm' | 'md' | 'lg'

const paddingMap: Record<CardPadding, string> = {
  none: '',
  sm: 'p-3',
  md: 'p-4',
  lg: 'p-6',
}

export interface CardProps extends Omit<HTMLAttributes<HTMLDivElement>, 'children'> {
  children: ReactNode
  padding?: CardPadding
}

export default function Card({
  children,
  padding = 'md',
  className = '',
  ...rest
}: CardProps) {
  const paddingClass = paddingMap[padding]
  return (
    <div
      className={`bg-white rounded-2xl shadow-card border border-slate-100 hover:shadow-card-hover transition-shadow duration-200 ${paddingClass} ${className}`.trim()}
      {...rest}
    >
      {children}
    </div>
  )
}
