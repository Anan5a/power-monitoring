import { useSearchParams } from 'react-router-dom'

export default function ResetPasswordPage() {
  const [searchParams] = useSearchParams()
  const token = searchParams.get('token')
  const type = searchParams.get('type')

  if (token && type === 'recovery') {
    return (
      <div className="min-h-screen flex items-center justify-center bg-gray-100">
        <div className="bg-white p-8 rounded-lg shadow-md w-96 text-center">
          <h2 className="text-xl font-bold mb-2 text-red-600">Reset via Supabase</h2>
          <p className="text-gray-600 mb-4">
            Password reset links are sent from the Supabase dashboard.
            Please use the link from the email you received.
          </p>
          <a href="/login" className="text-blue-600 hover:underline">Go to sign in →</a>
        </div>
      </div>
    )
  }

  return (
    <div className="min-h-screen flex items-center justify-center bg-gray-100">
      <div className="bg-white p-8 rounded-lg shadow-md w-96 text-center">
        <h2 className="text-xl font-bold mb-2">Password Reset</h2>
        <p className="text-gray-600 mb-4">
          Please request a password reset link from your administrator
          or contact support.
        </p>
        <a href="/login" className="text-blue-600 hover:underline">Go to sign in →</a>
      </div>
    </div>
  )
}