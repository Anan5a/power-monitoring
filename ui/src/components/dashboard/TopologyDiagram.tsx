interface TopologyDiagramProps {
  pvPower: number
  gridPower: number
  batteryPower: number
  loadPower: number
  batterySoc: number | null
}

function Node({
  label,
  value,
  unit,
  icon,
  color,
}: {
  label: string
  value: string
  unit?: string
  icon: React.ReactNode
  color: string
}) {
  return (
    <div className="flex flex-col items-center">
      <div className={`h-20 w-20 rounded-full border-4 ${color} flex items-center justify-center bg-white`}>
        {icon}
      </div>
      <div className="mt-2 text-center">
        <div className="text-sm font-semibold text-gray-700">{label}</div>
        <div className="text-lg font-bold text-gray-800">
          {value}
          {unit && <span className="text-sm font-normal text-gray-500 ml-0.5">{unit}</span>}
        </div>
      </div>
    </div>
  )
}

export default function TopologyDiagram({
  pvPower,
  gridPower,
  batteryPower,
  loadPower,
  batterySoc,
}: TopologyDiagramProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-6 shadow-sm">
      <div className="flex flex-col items-center gap-6 md:flex-row md:items-center md:justify-center md:gap-8">
        <Node label="PV" value={pvPower.toFixed(1)} unit="kW" icon={<span className="text-2xl">☀</span>} color="border-amber-400" />
        <div className="hidden md:block h-1 w-16 bg-amber-300" />
        <Node label="Inverter" value={(pvPower + batteryPower).toFixed(1)} unit="kW" icon={<span className="text-2xl">⚡</span>} color="border-orange-500" />
        <div className="hidden md:block h-1 w-16 bg-blue-300" />
        <Node label="Grid" value={gridPower.toFixed(1)} unit="kW" icon={<span className="text-2xl">🏭</span>} color="border-blue-400" />
      </div>
      <div className="flex flex-col md:flex-row items-center justify-center gap-8 mt-6">
        <Node label="Battery" value={batterySoc?.toFixed(0) ?? '--'} unit="%" icon={<span className="text-2xl">🔋</span>} color={batteryPower > 0 ? 'border-green-400' : 'border-amber-400'} />
        <Node label="Load" value={loadPower.toFixed(1)} unit="kW" icon={<span className="text-2xl">💡</span>} color="border-gray-400" />
      </div>
    </div>
  )
}