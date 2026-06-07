import { atom } from 'jotai'
import type { LayoutDoc } from '../lib/types'

const DEFAULT_LAYOUT: LayoutDoc = {
  version: 1,
  entries: [
    { id: 'qs',     type: 'quickstats',  gridArea: { col: 1,  row: 1,  colSpan: 12, rowSpan: 2 } },
    { id: 'relays', type: 'relays',      gridArea: { col: 1,  row: 3,  colSpan: 12, rowSpan: 1 } },
    { id: 'inv',    type: 'inverter',    gridArea: { col: 1,  row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'gen',    type: 'generation',  gridArea: { col: 4,  row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'bat',    type: 'battery',     gridArea: { col: 7,  row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'sp1',    type: 'spacer',      gridArea: { col: 10, row: 4,  colSpan: 3,  rowSpan: 2 } },
    { id: 'vc0',    type: 'vc0',         gridArea: { col: 1,  row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 0 } },
    { id: 'vc1',    type: 'vc1',         gridArea: { col: 4,  row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 1 } },
    { id: 'vc2',    type: 'vc2',         gridArea: { col: 7,  row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 2 } },
    { id: 'vc3',    type: 'vc3',         gridArea: { col: 10, row: 6,  colSpan: 3,  rowSpan: 2 }, props: { channel: 3 } },
    { id: 'hist',   type: 'history',     gridArea: { col: 1,  row: 8,  colSpan: 12, rowSpan: 6 } },
  ],
}

export const defaultLayoutAtom = atom<LayoutDoc>(DEFAULT_LAYOUT)