import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import TopologyDiagram from '../TopologyDiagram'

describe('TopologyDiagram', () => {
  it('renders all nodes with values', () => {
    render(
      <TopologyDiagram
        pvPower={1.5}
        gridPower={0.2}
        batteryPower={0.5}
        loadPower={0.8}
        batterySoc={75}
      />
    )

    expect(screen.getByText('PV')).toBeInTheDocument()
    expect(screen.getByText('Inverter')).toBeInTheDocument()
    expect(screen.getByText('Grid')).toBeInTheDocument()
    expect(screen.getByText('Battery')).toBeInTheDocument()
    expect(screen.getByText('Load')).toBeInTheDocument()
  })

  it('shows battery SoC', () => {
    render(
      <TopologyDiagram
        pvPower={0}
        gridPower={0}
        batteryPower={1}
        loadPower={0}
        batterySoc={86}
      />
    )

    expect(screen.getByText('86')).toBeInTheDocument()
  })
})