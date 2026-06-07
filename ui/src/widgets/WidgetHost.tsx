import { Suspense, lazy } from 'react'
import { useAtomValue } from 'jotai'
import type { LayoutEntry } from '../lib/types'
import { registry } from './registry'
import { selectedDeviceAtom } from '../state/atoms'

interface Props {
  entry: LayoutEntry
}

export default function WidgetHost({ entry }: Props) {
  const def = registry[entry.type]
  const device = useAtomValue(selectedDeviceAtom)
  if (!def) {
    return (
      <div className="h-full w-full rounded-2xl border border-red-200 bg-red-50 flex items-center justify-center text-red-500 text-sm">
        Unknown widget: {entry.type}
      </div>
    )
  }
  const Component = lazy(def.loader)
  // Inject deviceKey from the global atom so widgets don't need it in layout.props
  const props = { ...(entry.props ?? {}), ...(device?.device_key ? { deviceKey: device.device_key } : {}) }
  return (
    <Suspense fallback={<div className="h-full w-full bg-slate-50 animate-pulse rounded-2xl" />}>
      <Component {...props} />
    </Suspense>
  )
}
