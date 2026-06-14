import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import SolisDevicePage from '../SolisDevicePage'

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

describe('SolisDevicePage', () => {
  it('shows device selector prompt when no device selected', () => {
    render(
      <BrowserRouter>
        <SolisDevicePage />
      </BrowserRouter>
    )

    expect(screen.getByText('Select a device to view details.')).toBeInTheDocument()
  })
})