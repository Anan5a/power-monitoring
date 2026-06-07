import { Suspense, lazy } from 'react'
import type { LayoutEntry } from '../lib/types'
import { registry } from './registry'

interface Props {
  entry: LayoutEntry
}

export default function WidgetHost({ entry }: Props) {
  const def = registry[entry.type]
  if (!def) {
    return (
      <div className="h-full w-full rounded-2xl border border-red-200 bg-red-50 flex items-center justify-center text-red-500 text-sm">
        Unknown widget: {entry.type}
      </div>
    )
  }
  const Component = lazy(def.loader)
  return (
    <Suspense fallback={<div className="h-full w-full bg-slate-50 animate-pulse rounded-2xl" />}>
      <Component {...(entry.props ?? {})} />
    </Suspense>
  )
}
