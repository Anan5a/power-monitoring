import { useEffect } from 'react'
import { useAtomValue, useSetAtom } from 'jotai'
import { devicesAtom, selectedDeviceAtom } from '../state/atoms'
import { supabase } from './supabase'
import type { Device } from './types'

let loaded = false

export function useDevicesLoader() {
  const setDevices = useSetAtom(devicesAtom)
  const setSelectedDevice = useSetAtom(selectedDeviceAtom)
  const selectedDevice = useAtomValue(selectedDeviceAtom)

  useEffect(() => {
    if (loaded) return
    loaded = true
    supabase.from('devices').select('*').order('device_name').then(({ data }) => {
      if (!data || data.length === 0) return
      setDevices(data as Device[])
      if (!selectedDevice) {
        setSelectedDevice(data[0] as Device)
      }
    })
  }, [setDevices, setSelectedDevice, selectedDevice])
}