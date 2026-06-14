interface MetricGroup {
  label: string
  metrics: { key: string; label: string }[]
}

interface MetricSelectorProps {
  groups: MetricGroup[]
  selected: string[]
  onChange: (selected: string[]) => void
}

export default function MetricSelector({ groups, selected, onChange }: MetricSelectorProps) {
  function toggle(key: string) {
    onChange(selected.includes(key) ? selected.filter((k) => k !== key) : [...selected, key])
  }

  return (
    <div className="bg-white rounded-lg border border-gray-200 p-4 shadow-sm space-y-4">
      {groups.map((g) => (
        <div key={g.label}>
          <div className="text-sm font-semibold text-gray-700 mb-2">{g.label}</div>
          <div className="flex flex-wrap gap-3">
            {g.metrics.map((m) => (
              <label key={m.key} className="inline-flex items-center gap-1.5 text-sm text-gray-600 cursor-pointer">
                <input
                  type="checkbox"
                  checked={selected.includes(m.key)}
                  onChange={() => toggle(m.key)}
                  className="rounded border-gray-300"
                />
                {m.label}
              </label>
            ))}
          </div>
        </div>
      ))}
    </div>
  )
}