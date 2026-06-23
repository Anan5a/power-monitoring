import { useEffect, useRef } from 'react'
import { useAtomValue, useSetAtom, useStore } from 'jotai'
import { devicesAtom, selectedDeviceAtom, devicesLoadingAtom } from '../state/atoms'
import { supabase } from './supabase'
import type { Device } from './types'

export function useDevicesLoader() {
  const store = useStore()
  const setDevices = useSetAtom(devicesAtom)
  const setSelectedDevice = useSetAtom(selectedDeviceAtom)
  const setDevicesLoading = useSetAtom(devicesLoadingAtom)
  const selectedDevice = useAtomValue(selectedDeviceAtom)
  const fetchedRef = useRef(false)

  useEffect(() => {
    if (fetchedRef.current) return
    fetchedRef.current = true

    supabase.from('devices').select('*').order('device_name')
      .then(({ data, error }) => {
        if (error) {
          console.error('useDevicesLoader: query error', error)
          setDevicesLoading(false)
          return
        }
        if (!data || data.length === 0) {
          setDevicesLoading(false)
          return
        }
        setDevices(data as Device[])
        // Read current value from store without subscribing to avoid
        // re-triggering this effect when selectedDevice changes.
        if (!store.get(selectedDeviceAtom)) {
          setSelectedDevice(data[0] as Device)
        }
        setDevicesLoading(false)
      })
      .catch((err) => {
        console.error('useDevicesLoader: unexpected error', err)
        setDevicesLoading(false)
      })
    // Intentionally run once — fetchedRef guards against re-execution.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [setDevices, setSelectedDevice, setDevicesLoading, store])
}
