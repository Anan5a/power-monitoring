import { useEffect, useRef } from 'react'
import { useStore } from 'jotai'
import { loadChannels } from '../state/services/channelsService'
import type { Device } from './types'

export function useTelemetryInit(selectedDevice: Device | null) {
  const store = useStore()
  const prevKey = useRef<string | null>(null)

  useEffect(() => {
    if (!selectedDevice) return
    if (prevKey.current === selectedDevice.device_key) return
    prevKey.current = selectedDevice.device_key

    const deviceKey = selectedDevice.device_key

    Promise.all([
      import('../state/services/telemetryService').then((m) => m.startLiveTelemetry(store, deviceKey)),
      loadChannels(store, deviceKey),
    ])

    return () => {
      import('../state/services/telemetryService').then((m) => m.stopLiveTelemetry())
      prevKey.current = null
    }
  }, [selectedDevice, store])
}
