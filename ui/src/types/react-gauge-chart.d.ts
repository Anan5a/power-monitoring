declare module 'react-gauge-chart' {
  import type { CSSProperties, ReactElement } from 'react'

  interface GaugeChartProps {
    id: string
    className?: string
    style?: CSSProperties
    marginInPercent?: number
    cornerRadius?: number
    nrOfLevels?: number
    percent?: number
    arcPadding?: number
    arcWidth?: number
    arcsLength?: number[]
    colors?: string[]
    textColor?: string
    needleColor?: string
    needleBaseColor?: string
    hideText?: boolean
    animate?: boolean
    formatTextValue?: (value: string) => string
    fontSize?: string
    animateDuration?: number
    animDelay?: number
    textComponent?: ReactElement
    textComponentContainerClassName?: string
    needleScale?: number
  }

  export default function GaugeChart(props: GaugeChartProps): ReactElement
}
