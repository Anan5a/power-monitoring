import { useNavigate } from 'react-router-dom'
import { useAtomValue } from 'jotai'
import { connectionStateAtom } from '../state/atoms'

export default function LegacyDashboardPage() {
  const navigate = useNavigate()
  const conn = useAtomValue(connectionStateAtom)
  return (
    <div className="min-h-screen flex flex-col items-center justify-center p-8 bg-slate-50 text-slate-600">
      <div className="max-w-md text-center space-y-3">
        <h1 className="text-2xl font-bold text-slate-800">Legacy Dashboard</h1>
        <p className="text-sm">The legacy dashboard has been deprecated in favor of the redesigned dashboard at <a href="/dashboard" className="text-cyan-600 underline">/dashboard</a>.</p>
        <p className="text-xs text-slate-400">This route is preserved as a fallback. Connection: {conn}</p>
        <button onClick={() => navigate('/dashboard')} className="mt-4 px-4 py-2 bg-cyan-500 text-white rounded-lg text-sm font-semibold hover:bg-cyan-600">
          Go to new dashboard
        </button>
      </div>
    </div>
  )
}
