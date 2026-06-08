import { useAtomValue } from 'jotai'
import { layoutAtom } from '../state/atoms'
import { defaultLayoutAtom } from '../state/layout'
import WidgetHost from './WidgetHost'

const ROW_HEIGHT = '80px'

// Responsive col spans: on small screens, the 12-col grid is too cramped.
// We map the layout's colSpan to a CSS class that adjusts at breakpoints.
function colSpanClass(colSpan: number): string {
  // The widget wants 1-12 of 12. We translate to a responsive class.
  // Mobile: collapse everything to 12 (single col)
  // sm (640px): 6-col effective (2 columns)
  // md (768px): 8-col effective
  // lg (1024px+): full 12-col layout
  if (colSpan >= 12) return 'col-span-12'
  if (colSpan >= 9) return 'col-span-12 sm:col-span-12 lg:col-span-12'
  if (colSpan >= 6) return 'col-span-12 sm:col-span-6 lg:col-span-6'
  if (colSpan >= 4) return 'col-span-12 sm:col-span-6 md:col-span-4 lg:col-span-4'
  if (colSpan >= 3) return 'col-span-12 sm:col-span-6 md:col-span-4 lg:col-span-3'
  return 'col-span-12 sm:col-span-6 md:col-span-4 lg:col-span-3'
}

function rowSpanStyle(rowSpan: number): React.CSSProperties {
  // min height = rowSpan * 80px + (rowSpan-1)*16px (gap)
  return { minHeight: `calc(${rowSpan} * 80px + (${rowSpan} - 1) * 16px)` }
}

export default function WidgetGrid() {
  const saved = useAtomValue(layoutAtom)
  const fallback = useAtomValue(defaultLayoutAtom)
  const doc = saved ?? fallback

  return (
    <div className="grid grid-cols-12 gap-4">
      {doc.entries.map(entry => (
        <div
          key={entry.id}
          className={colSpanClass(entry.gridArea.colSpan)}
          style={rowSpanStyle(entry.gridArea.rowSpan)}
        >
          <WidgetHost entry={entry} />
        </div>
      ))}
    </div>
  )
}
