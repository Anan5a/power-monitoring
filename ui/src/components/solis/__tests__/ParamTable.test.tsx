import { describe, it, expect } from 'vitest'
import { render, screen } from '@testing-library/react'
import ParamTable from '../ParamTable'

describe('ParamTable', () => {
  it('renders title, columns, and rows', () => {
    render(
      <ParamTable
        title="PV Section"
        columns={[
          { key: 'string', label: 'String' },
          { key: 'power', label: 'Power (kW)' },
        ]}
        rows={[
          { string: 'PV1', power: '1.5' },
          { string: 'PV2', power: '2.3' },
        ]}
      />
    )

    expect(screen.getByText('PV Section')).toBeInTheDocument()
    expect(screen.getByText('String')).toBeInTheDocument()
    expect(screen.getByText('PV1')).toBeInTheDocument()
    expect(screen.getByText('2.3')).toBeInTheDocument()
  })
})