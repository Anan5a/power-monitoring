import { describe, it, expect, vi } from 'vitest'
import { render, screen } from '@testing-library/react'
import SolisSidebar from '../SolisSidebar'

const noop = vi.fn()

describe('SolisSidebar', () => {
  it('renders navigation items and version', () => {
    render(
      <SolisSidebar
        currentPath="/dashboard"
        onNavigate={noop}
        onSignOut={noop}
        version="0.3.0"
      />
    )

    expect(screen.getByText('Overview')).toBeInTheDocument()
    expect(screen.getByText('Device')).toBeInTheDocument()
    expect(screen.getByText('Analysis')).toBeInTheDocument()
    expect(screen.getByText('Alarms')).toBeInTheDocument()
    expect(screen.getByText('Classic View')).toBeInTheDocument()
    expect(screen.getByText('v0.3.0')).toBeInTheDocument()
  })
})
