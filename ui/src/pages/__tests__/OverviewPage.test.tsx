import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import OverviewPage from '../OverviewPage'

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

describe('OverviewPage', () => {
  it('shows device selector prompt when no device selected', () => {
    render(
      <BrowserRouter>
        <OverviewPage />
      </BrowserRouter>
    )

    expect(screen.getByText('Select a device to view telemetry.')).toBeInTheDocument()
  })
})