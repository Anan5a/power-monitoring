import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import KpiCard from '../KpiCard'

describe('KpiCard', () => {
  it('renders label, value, unit, and subText', () => {
    render(
      <KpiCard
        label="Current Power"
        value="1.5"
        unit="W"
        subText="Installed Capacity: 4.2 kWp"
      />
    )

    expect(screen.getByText('Current Power')).toBeInTheDocument()
    expect(screen.getByText('1.5')).toBeInTheDocument()
    expect(screen.getByText('W')).toBeInTheDocument()
    expect(screen.getByText('Installed Capacity: 4.2 kWp')).toBeInTheDocument()
  })
})
