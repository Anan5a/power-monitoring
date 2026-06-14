import { describe, it, expect, vi } from 'vitest'
import { render, screen, fireEvent } from '@testing-library/react'
import DateRangeTabs from '../DateRangeTabs'

describe('DateRangeTabs', () => {
  it('renders all range options', () => {
    render(<DateRangeTabs value="day" onChange={vi.fn()} />)
    expect(screen.getByText('Day')).toBeInTheDocument()
    expect(screen.getByText('Month')).toBeInTheDocument()
    expect(screen.getByText('Year')).toBeInTheDocument()
    expect(screen.getByText('Total')).toBeInTheDocument()
  })

  it('calls onChange when a tab is clicked', () => {
    const onChange = vi.fn()
    render(<DateRangeTabs value="day" onChange={onChange} />)
    fireEvent.click(screen.getByText('Month'))
    expect(onChange).toHaveBeenCalledWith('month')
  })
})
