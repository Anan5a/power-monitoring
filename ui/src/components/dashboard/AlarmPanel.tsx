interface AlarmPanelProps {
  alarmCount: number
}

export default function AlarmPanel({ alarmCount }: AlarmPanelProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 p-5 shadow-sm">
      <h3 className="text-sm font-semibold text-gray-700 mb-3">Alarm</h3>
      {alarmCount === 0 ? (
        <div className="text-sm text-gray-500">No alarm</div>
      ) : (
        <div className="text-sm text-amber-600">{alarmCount} active alarms</div>
      )}
    </div>
  )
}