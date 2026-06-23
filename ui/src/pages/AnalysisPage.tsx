import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAtom, useAtomValue } from 'jotai'
import { selectedDeviceAtom, devicesAtom, devicesLoadingAtom } from '../state/atoms'
import { liveBufferAtom, secondsAgoAtom } from '../state/derived'
import { supabase } from '../lib/supabase'
import { APP_VERSION } from '../lib/version'
import { useTelemetryInit } from '../lib/useTelemetryInit'
import DashboardShell from '../components/dashboard/DashboardShell'
import DateRangeTabs from '../components/dashboard/DateRangeTabs'
import MetricSelector from '../components/dashboard/MetricSelector'
import SolisChart from '../components/dashboard/DashboardChart'

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
  const devices = useAtomValue(devicesAtom)
  const devicesLoading = useAtomValue(devicesLoadingAtom)
  const [selectedDevice, setSelectedDevice] = useAtom(selectedDeviceAtom)
  const buffer = useAtomValue(liveBufferAtom)
  const secondsAgo = useAtomValue(secondsAgoAtom)
  useTelemetryInit(selectedDevice)
  const [range, setRange] = useState<Range>('day')
  const [selectedMetrics, setSelectedMetrics] = useState<string[]>(['total_power'])

  const online = selectedDevice?.is_online ?? false

  function handleNavigate(path: string) { navigate(path) }
  function handleSignOut() { supabase.auth.signOut().then(() => navigate('/login')) }
  function handleRefresh() { window.location.reload() }

  if (devicesLoading) {
    return (
      <DashboardShell
        currentPath="/dashboard/analysis"
        onNavigate={handleNavigate}
        onSignOut={handleSignOut}
        devices={devices}
        selectedDeviceId={null}
        onSelectDevice={setSelectedDevice}
        isOnline={false}
        version={APP_VERSION}
        lastUpdated={secondsAgo != null ? `${secondsAgo}s ago` : undefined}
        onRefresh={handleRefresh}
      >
        <div className="text-center py-20 text-gray-500">Loading devices...</div>
      </DashboardShell>
    )
  }

  if (!selectedDevice) {
    return (
      <DashboardShell
        currentPath="/dashboard/analysis"
        onNavigate={handleNavigate}
        onSignOut={handleSignOut}
        devices={devices}
        selectedDeviceId={null}
        onSelectDevice={setSelectedDevice}
        isOnline={false}
        version={APP_VERSION}
        lastUpdated={secondsAgo != null ? `${secondsAgo}s ago` : undefined}
        onRefresh={handleRefresh}
      >
        <div className="text-center py-20 text-gray-500">Select a device to view analysis.</div>
      </DashboardShell>
    )
  }

  return (
    <DashboardShell
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
    </DashboardShell>
  )
}