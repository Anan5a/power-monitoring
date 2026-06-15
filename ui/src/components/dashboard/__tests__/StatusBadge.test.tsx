import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import StatusBadge from '../StatusBadge'

describe('StatusBadge', () => {
  it('renders dot and label for normal status', () => {
    render(<StatusBadge status="normal" />)
    expect(screen.getByText('normal')).toBeInTheDocument()
  })

  it('renders custom label', () => {
    render(<StatusBadge status="normal" label="Online" />)
    expect(screen.getByText('Online')).toBeInTheDocument()
  })
})
