import type { Device } from '../lib/types'

interface Props {
  fields: Array<{ key: string; label: string; unit: string; chart: boolean }>
  selected: string[]
  onChange: (keys: string[]) => void
}

export default function ChannelSelector({ fields, selected, onChange }: Props) {
  function toggle(key: string) {
    if (selected.includes(key)) {
      onChange(selected.filter(k => k !== key))
    } else {
      onChange([...selected, key])
    }
  }

  return (
    <div className="flex flex-wrap gap-2">
      {fields.map(field => (
        <label key={field.key} className="inline-flex items-center gap-1.5 cursor-pointer">
          <input
            type="checkbox"
            checked={selected.includes(field.key)}
            onChange={() => toggle(field.key)}
            className="rounded border-gray-300 text-blue-600 focus:ring-blue-500"
          />
          <span className="text-sm text-gray-700">
            {field.label}
            {field.unit && <span className="text-gray-400 text-xs ml-1">({field.unit})</span>}
          </span>
        </label>
      ))}
    </div>
  )
}