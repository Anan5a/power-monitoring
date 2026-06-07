import { useAtomValue } from 'jotai'
import { layoutAtom } from '../state/atoms'
import { defaultLayoutAtom } from '../state/layout'
import WidgetHost from './WidgetHost'

const ROW_HEIGHT = '80px'

export default function WidgetGrid() {
  const saved = useAtomValue(layoutAtom)
  const fallback = useAtomValue(defaultLayoutAtom)
  const doc = saved ?? fallback

  return (
    <div
      className="grid gap-4"
      style={{
        gridTemplateColumns: 'repeat(12, minmax(0, 1fr))',
        gridAutoRows: ROW_HEIGHT,
      }}
    >
      {doc.entries.map(entry => (
        <div
          key={entry.id}
          style={{
            gridColumn: `${entry.gridArea.col} / span ${entry.gridArea.colSpan}`,
            gridRow: `${entry.gridArea.row} / span ${entry.gridArea.rowSpan}`,
            minHeight: 0,
          }}
        >
          <WidgetHost entry={entry} />
        </div>
      ))}
    </div>
  )
}
