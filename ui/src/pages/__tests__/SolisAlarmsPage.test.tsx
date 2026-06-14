import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import SolisAlarmsPage from '../SolisAlarmsPage'

vi.mock('../../lib/supabase', () => ({
  supabase: {
    from: vi.fn(() => ({
      select: vi.fn(() => ({
        order: vi.fn().mockResolvedValue({ data: [], error: null }),
      })),
    })),
    auth: {
      signOut: vi.fn().mockResolvedValue({}),
    },
  },
}))

describe('SolisAlarmsPage', () => {
  it('renders the alarms heading', () => {
    render(
      <BrowserRouter>
        <SolisAlarmsPage />
      </BrowserRouter>
    )

    expect(screen.getByRole('heading', { name: 'Alarms', level: 2 })).toBeInTheDocument()
  })

  it('shows offline alarm when no device selected', () => {
    render(
      <BrowserRouter>
        <SolisAlarmsPage />
      </BrowserRouter>
    )

    expect(screen.getByText('CRITICAL')).toBeInTheDocument()
    expect(screen.getByText(/Device is offline/)).toBeInTheDocument()
  })
})