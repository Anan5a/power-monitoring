import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import { Provider, createStore } from 'jotai'
import { devicesLoadingAtom } from '../../state/atoms'
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

function renderWithStore() {
  const store = createStore()
  // The page branches on devicesLoading first; the test wants to assert the
  // "no device selected" empty state, so we have to flip it off. In a real
  // run this is set by useDevicesLoader in App.tsx, which isn't mounted here.
  store.set(devicesLoadingAtom, false)
  return render(
    <Provider store={store}>
      <BrowserRouter>
        <OverviewPage />
      </BrowserRouter>
    </Provider>,
  )
}

describe('OverviewPage', () => {
  it('shows device selector prompt when no device selected', () => {
    renderWithStore()

    expect(screen.getByText('Select a device to view telemetry.')).toBeInTheDocument()
  })
})