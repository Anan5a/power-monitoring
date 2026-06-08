// Custom HTML tooltip removed — Chart.js's built-in tooltip handles hover
// via the configured callbacks in chartOptions. The old React overlay
// duplicated data and rendered '[object Object]' due to a key/label
// mismatch. Keeping this file as a no-op export so imports don't break.
import { memo } from 'react'

function HistoryTooltip() {
  return null
}

export default memo(HistoryTooltip)