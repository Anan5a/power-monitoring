import { useEffect } from 'react'
import { useSetAtom } from 'jotai'
import { devicesAtom } from '../state/atoms'
import { supabase } from './supabase'
import type { Device } from './types'

let loaded = false

export function useDevicesLoader() {
  const setDevices = useSetAtom(devicesAtom)

  useEffect(() => {
    if (loaded) return
    loaded = true
    supabase.from('devices').select('*').order('device_name').then(({ data }) => {
      if (data) setDevices(data as Device[])
    })
  }, [setDevices])
}
