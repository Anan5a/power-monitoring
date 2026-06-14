import { describe, it, expect } from 'vitest'
import { render } from '@testing-library/react'
import SemiGauge from '../SemiGauge'

describe('SemiGauge', () => {
  it('renders an SVG with two arc paths', () => {
    const { container } = render(<SemiGauge value={50} max={100} />)
    const paths = container.querySelectorAll('path')
    expect(paths.length).toBe(2)
  })

  it('renders with custom color', () => {
    const { container } = render(<SemiGauge value={50} max={100} color="#22c55e" />)
    const paths = container.querySelectorAll('path')
    expect(paths[1].getAttribute('stroke')).toBe('#22c55e')
  })
})
