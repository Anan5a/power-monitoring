import { describe, it, expect, vi } from 'vitest'
import { render, screen, fireEvent } from '@testing-library/react'
import MetricSelector from '../MetricSelector'

const groups = [
  {
    label: 'DC',
    metrics: [
      { key: 'ch0_V', label: 'Voltage' },
      { key: 'ch0_I', label: 'Current' },
    ],
  },
  {
    label: 'Output',
    metrics: [
      { key: 'total_power', label: 'Total Power' },
    ],
  },
]

describe('MetricSelector', () => {
  it('renders groups and checkboxes', () => {
    render(<MetricSelector groups={groups} selected={['ch0_V']} onChange={vi.fn()} />)

    expect(screen.getByText('DC')).toBeInTheDocument()
    expect(screen.getByText('Voltage')).toBeInTheDocument()
    expect(screen.getByText('Current')).toBeInTheDocument()
    expect(screen.getByText('Total Power')).toBeInTheDocument()
  })

  it('calls onChange when a checkbox is clicked', () => {
    const onChange = vi.fn()
    render(<MetricSelector groups={groups} selected={[]} onChange={onChange} />)
    fireEvent.click(screen.getByLabelText('Voltage'))
    expect(onChange).toHaveBeenCalledWith(['ch0_V'])
  })

  it('unchecks an already selected item', () => {
    const onChange = vi.fn()
    render(<MetricSelector groups={groups} selected={['ch0_V']} onChange={onChange} />)
    fireEvent.click(screen.getByLabelText('Voltage'))
    expect(onChange).toHaveBeenCalledWith([])
  })
})