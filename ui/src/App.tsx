import { BrowserRouter, Routes, Route, Navigate, useLocation } from 'react-router-dom'
import { useEffect, useState } from 'react'
import { supabase } from './lib/supabase'
import type { Session } from '@supabase/supabase-js'
import LoginPage from './pages/LoginPage'
import ClassicDashboardPage from './pages/ClassicDashboardPage'
import OverviewPage from './pages/OverviewPage'
import DevicePage from './pages/DevicePage'
import AnalysisPage from './pages/AnalysisPage'
import AlarmsPage from './pages/AlarmsPage'
import AdminPage from './pages/AdminPage'
import ProvisioningPage from './pages/ProvisioningPage'
import ResetPasswordPage from './pages/ResetPasswordPage'
import SettingsPage from './pages/SettingsPage'
import ChannelsPage from './pages/ChannelsPage'
import { useNowTicker } from './state/nowTicker'
import { useDevicesLoader } from './lib/useDevicesLoader'
import ShellWrap from './components/ShellWrap'

function ProtectedRoute({ children }: { children: React.ReactNode }) {
  const [session, setSession] = useState<Session | null>(null)
  const [loading, setLoading] = useState(true)
  const location = useLocation()
  useEffect(() => {
    supabase.auth.getSession().then(({ data: { session } }) => {
      setSession(session)
      setLoading(false)
    })
  }, [])
  if (loading) return (
    <div className="flex items-center justify-center h-screen bg-gray-100">
      <div className="text-lg text-gray-600">Loading...</div>
    </div>
  )
  return session ? <>{children}</> : <Navigate to="/login" state={{ from: location }} replace />
}

export default function App() {
  useNowTicker()
  useDevicesLoader()
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/provision" element={<ProvisioningPage />} />
        <Route path="/reset-password" element={<ResetPasswordPage />} />
        <Route path="/dashboard" element={<ProtectedRoute><OverviewPage /></ProtectedRoute>} />
        <Route path="/dashboard/device" element={<ProtectedRoute><DevicePage /></ProtectedRoute>} />
        <Route path="/dashboard/analysis" element={<ProtectedRoute><AnalysisPage /></ProtectedRoute>} />
        <Route path="/dashboard/alarms" element={<ProtectedRoute><AlarmsPage /></ProtectedRoute>} />
        <Route path="/dashboard/classic" element={<ProtectedRoute><ClassicDashboardPage /></ProtectedRoute>} />
        <Route path="/admin" element={<ProtectedRoute><ShellWrap currentPath="/admin"><AdminPage /></ShellWrap></ProtectedRoute>} />
        <Route path="/settings" element={<ProtectedRoute><ShellWrap currentPath="/settings"><SettingsPage /></ShellWrap></ProtectedRoute>} />
        <Route path="/channels" element={<ProtectedRoute><ShellWrap currentPath="/channels"><ChannelsPage /></ShellWrap></ProtectedRoute>} />
        <Route path="/" element={<Navigate to="/dashboard" replace />} />
      </Routes>
    </BrowserRouter>
  )
}