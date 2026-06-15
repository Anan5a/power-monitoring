import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import AlarmsPage from '../AlarmsPage'

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

describe('AlarmsPage', () => {
  it('renders the alarms heading', () => {
    render(
      <BrowserRouter>
        <AlarmsPage />
      </BrowserRouter>
    )

    expect(screen.getByRole('heading', { name: 'Alarms', level: 2 })).toBeInTheDocument()
  })

  it('shows offline alarm when no device selected', () => {
    render(
      <BrowserRouter>
        <AlarmsPage />
      </BrowserRouter>
    )

    expect(screen.getByText('CRITICAL')).toBeInTheDocument()
    expect(screen.getByText(/Device is offline/)).toBeInTheDocument()
  })
})