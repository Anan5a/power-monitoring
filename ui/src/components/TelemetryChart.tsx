import { useEffect, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'
import type { TelemetryPoint } from '../lib/types'

interface Props {
  data: TelemetryPoint[]
  dataKey: string
  color?: string
}

export default function TelemetryChart({ data, dataKey, color = 'blue' }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<uPlot | null>(null)

  useEffect(() => {
    if (!containerRef.current || data.length === 0) return

    const timestamps = data.map(p => new Date(p.recorded_at).getTime() / 1000)
    const values = data.map(p => (p.payload?.[dataKey] as number) ?? 0)

    const container = containerRef.current
    const width = container.clientWidth || 400

    const opts: uPlot.Options = {
      width,
      height: 200,
      series: [
        {},
        { stroke: color, width: 2 },
      ],
      axes: [
        {},
        { stroke: '#94a3b8', font: '10px sans-serif' },
      ],
      cursor: { show: true },
    }

    const plot = new uPlot(opts, [timestamps, values], container)
    plotRef.current = plot

    return () => {
      plot.destroy()
      plotRef.current = null
    }
  }, [data, dataKey, color])

  return (
    <div ref={containerRef} className="w-full" />
  )
}