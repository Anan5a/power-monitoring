import { describe, it, expect } from 'vitest'
import { render } from '@testing-library/react'
import SemiGauge from '../SemiGauge'

describe('SemiGauge', () => {
  it('renders without crashing', () => {
    const { container } = render(<SemiGauge value={50} max={100} />)
    expect(container.querySelector('svg')).toBeTruthy()
  })

  it('renders with custom color', () => {
    const { container } = render(<SemiGauge value={50} max={100} color="#22c55e" />)
    expect(container.querySelector('svg')).toBeTruthy()
  })
})
