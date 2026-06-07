import type { ComponentType } from 'react'
import type { WidgetType, WidgetDef } from '../lib/types'

// Lazy imports for code-splitting per widget
const QuickStatsWidget = () => import('./QuickStatsWidget')
const RelaysWidget = () => import('./RelaysWidget')
const InverterWidget = () => import('./InverterWidget')
const GenerationWidget = () => import('./GenerationWidget')
const BatteryWidget = () => import('./BatteryWidget')
const VCCardWidget = () => import('./VCCardWidget')
const HistoryChartWidget = () => import('./HistoryChartWidget')
const SpacerWidget = () => import('./SpacerWidget')
const PlaceholderWidget = () => import('./PlaceholderWidget')

export const registry: Record<WidgetType, WidgetDef & { loader: () => Promise<{ default: ComponentType<any> }> }> = {
  quickstats: {
    type: 'quickstats',
    label: 'Quick Stats',
    defaultSize: { colSpan: 12, rowSpan: 2 },
    loader: QuickStatsWidget,
  },
  relays: {
    type: 'relays',
    label: 'Relays',
    defaultSize: { colSpan: 12, rowSpan: 1 },
    loader: RelaysWidget,
  },
  inverter: {
    type: 'inverter',
    label: 'Inverter',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: InverterWidget,
  },
  generation: {
    type: 'generation',
    label: 'Generation',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: GenerationWidget,
  },
  battery: {
    type: 'battery',
    label: 'Battery',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: BatteryWidget,
  },
  vc0: {
    type: 'vc0',
    label: 'VC 0',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 0 },
    loader: VCCardWidget,
  },
  vc1: {
    type: 'vc1',
    label: 'VC 1',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 1 },
    loader: VCCardWidget,
  },
  vc2: {
    type: 'vc2',
    label: 'VC 2',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 2 },
    loader: VCCardWidget,
  },
  vc3: {
    type: 'vc3',
    label: 'VC 3',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    defaultProps: { channel: 3 },
    loader: VCCardWidget,
  },
  history: {
    type: 'history',
    label: 'History',
    defaultSize: { colSpan: 12, rowSpan: 6 },
    loader: HistoryChartWidget,
  },
  spacer: {
    type: 'spacer',
    label: 'Spacer',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: SpacerWidget,
  },
  placeholder: {
    type: 'placeholder',
    label: 'Placeholder',
    defaultSize: { colSpan: 3, rowSpan: 2 },
    loader: PlaceholderWidget,
  },
}