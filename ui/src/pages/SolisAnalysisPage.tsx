import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useAtomValue } from 'jotai'
import { selectedDeviceAtom } from '../state/atoms'
import { liveBufferAtom } from '../state/derived'
import { supabase } from '../lib/supabase'
import type { Device } from '../lib/types'
import { APP_VERSION } from '../lib/version'
import SolisLayout from '../components/solis/SolisLayout'
import DateRangeTabs from '../components/solis/DateRangeTabs'
import MetricSelector from '../components/solis/MetricSelector'
import SolisChart from '../components/solis/SolisChart'

type Range = 'day' | 'month' | 'year' | 'total'

const GROUPS = [
  {
    label: 'DC',
    metrics: [
      { key: 'ch0_V', label: 'Voltage' },
      { key: 'ch0_I', label: 'Current' },
      { key: 'ch0_P', label: 'Power' },
    ],
  },
  {
    label: 'AC',
    metrics: [
      { key: 'ac_voltage', label: 'Voltage' },
      { key: 'ac_current', label: 'Current' },
      { key: 'ac_frequency', label: 'Frequency' },
    ],
  },
  {
    label: 'Output',
    metrics: [
      { key: 'total_power', label: 'Total Power' },
      { key: 'daily_yield', label: 'Daily Yield' },
      { key: 'total_yield', label: 'Total Yield' },
    ],
  },
]

export default function SolisAnalysisPage() {
  const navigate = useNavigate()
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const buffer = useAtomValue(liveBufferAtom)
  const [range, setRange] = useState<Range>('day')
  const [selectedMetrics, setSelectedMetrics] = useState<string[]>(['total_power'])

  useEffect(() => {
    async function load() {
      const { data } = await supabase.from('devices').select('*').order('device_name')
      if (data) setDevices(data)
    }
    load()
  }, [])

  const online = selectedDevice?.is_online ?? false

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  if (!selectedDevice) {
    return (
      <SolisLayout
        currentPath="/dashboard/analysis"
        onNavigate={handleNavigate}
        onSignOut={handleSignOut}
        devices={devices}
        selectedDeviceId={null}
        onSelectDevice={setSelectedDevice}
        isOnline={false}
        version={APP_VERSION}
        onRefresh={handleRefresh}
      >
        <div className="text-center py-20 text-gray-500">Select a device to view analysis.</div>
      </SolisLayout>
    )
  }

  return (
    <SolisLayout
      currentPath="/dashboard/analysis"
      onNavigate={handleNavigate}
      onSignOut={handleSignOut}
      devices={devices}
      selectedDeviceId={selectedDevice.id}
      onSelectDevice={setSelectedDevice}
      isOnline={online}
      version={APP_VERSION}
      onRefresh={handleRefresh}
    >
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-lg font-semibold text-gray-800">Analysis</h2>
        <DateRangeTabs value={range} onChange={setRange} />
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-4 gap-4 mb-4">
        <div className="lg:col-span-1">
          <MetricSelector groups={GROUPS} selected={selectedMetrics} onChange={setSelectedMetrics} />
        </div>
        <div className="lg:col-span-3 space-y-4">
          {selectedMetrics.length === 0 ? (
            <div className="bg-white rounded-lg border border-gray-200 p-8 shadow-sm text-center text-gray-400">
              Select metrics to display.
            </div>
          ) : (
            selectedMetrics.map((metric) => (
              <SolisChart key={metric} data={buffer} metric={metric} />
            ))
          )}
        </div>
      </div>
    </SolisLayout>
  )
}