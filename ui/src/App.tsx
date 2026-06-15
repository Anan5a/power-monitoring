import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom'
import { useEffect, useState } from 'react'
import { supabase } from './lib/supabase'
import type { Session } from '@supabase/supabase-js'
import LoginPage from './pages/LoginPage'
import ClassicDashboardPage from './pages/ClassicDashboardPage'
import SolisOverviewPage from './pages/SolisOverviewPage'
import SolisDevicePage from './pages/SolisDevicePage'
import SolisAnalysisPage from './pages/SolisAnalysisPage'
import SolisAlarmsPage from './pages/SolisAlarmsPage'
import AdminPage from './pages/AdminPage'
import ProvisioningPage from './pages/ProvisioningPage'
import ResetPasswordPage from './pages/ResetPasswordPage'
import SettingsPage from './pages/SettingsPage'
import ChannelsPage from './pages/ChannelsPage'
import { useNowTicker } from './state/nowTicker'
import { useDevicesLoader } from './lib/useDevicesLoader'

function ProtectedRoute({ children }: { children: React.ReactNode }) {
  const [session, setSession] = useState<Session | null>(null)
  const [loading, setLoading] = useState(true)
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
  return session ? <>{children}</> : <Navigate to="/login" replace />
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
        <Route path="/dashboard" element={<ProtectedRoute><SolisOverviewPage /></ProtectedRoute>} />
        <Route path="/dashboard/device" element={<ProtectedRoute><SolisDevicePage /></ProtectedRoute>} />
        <Route path="/dashboard/analysis" element={<ProtectedRoute><SolisAnalysisPage /></ProtectedRoute>} />
        <Route path="/dashboard/alarms" element={<ProtectedRoute><SolisAlarmsPage /></ProtectedRoute>} />
        <Route path="/dashboard/classic" element={<ProtectedRoute><ClassicDashboardPage /></ProtectedRoute>} />
        <Route path="/admin" element={<ProtectedRoute><AdminPage /></ProtectedRoute>} />
        <Route path="/settings" element={<ProtectedRoute><SettingsPage /></ProtectedRoute>} />
        <Route path="/channels" element={<ProtectedRoute><ChannelsPage /></ProtectedRoute>} />
        <Route path="/" element={<Navigate to="/dashboard" replace />} />
      </Routes>
    </BrowserRouter>
  )
}