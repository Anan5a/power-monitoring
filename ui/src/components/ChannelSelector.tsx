import type { ChannelGroup, ChannelName } from '../lib/types'

interface Props {
  fields: Array<{ key: string; label: string; unit: string; chart: boolean }>
  groups: ChannelGroup[]
  channelNames: ChannelName[]
  selected: string[]
  onChange: (keys: string[]) => void
}

const ICONS = ['☀️', '🔋', '⚡', '📟']

function resolveLabel(channelIndex: number, fieldKey: string, fields: Props['fields'], channelNames: ChannelName[]): string {
  const override = channelNames.find(c => c.channel === channelIndex)
  if (override?.name) return override.name
  const field = fields.find(f => f.key === fieldKey)
  return field?.label ?? fieldKey
}

function channelIndexFromKey(key: string): number {
  const m = key.match(/[0-9]+$/)
  return m ? parseInt(m[0], 10) : -1
}

export default function ChannelSelector({ fields, groups, channelNames, selected, onChange }: Props) {
  function toggle(key: string) {
    if (selected.includes(key)) {
      onChange(selected.filter(k => k !== key))
    } else {
      onChange([...selected, key])
    }
  }

  // Fields not yet placed in a group (by channel_mask bit check)
  const usedKeys = new Set(groups.flatMap(g => {
    const keys: string[] = []
    for (let ch = 0; ch < 4; ch++) {
      if (g.channel_mask & (1 << ch)) {
        const f = fields.find(f => f.key.endsWith(String(ch)))
        if (f) keys.push(f.key)
      }
    }
    return keys
  }))
  const ungroupedFields = fields.filter(f => !usedKeys.has(f.key))

  // Group-based layout when groups exist, otherwise flat list
  const hasGroups = groups.length > 0

  return (
    <div className="space-y-3">
      {hasGroups ? (
        groups.map(group => {
          const channelsInGroup = [0, 1, 2, 3].filter(ch => group.channel_mask & (1 << ch))
          return (
            <div key={group.group_id} className="border rounded-lg p-3">
              <div className="flex items-center gap-2 mb-2 font-medium text-gray-700">
                <span>{ICONS[group.icon] ?? '📟'}</span>
                <span>{group.name}</span>
              </div>
              <div className="flex flex-wrap gap-2">
                {channelsInGroup.map(ch => {
                  const field = fields.find(f => f.key.endsWith(String(ch)))
                  if (!field) return null
                  const label = resolveLabel(ch, field.key, fields, channelNames)
                  return (
                    <label key={field.key} className="inline-flex items-center gap-1.5 cursor-pointer">
                      <input
                        type="checkbox"
                        checked={selected.includes(field.key)}
                        onChange={() => toggle(field.key)}
                        className="rounded border-gray-300 text-blue-600 focus:ring-blue-500"
                      />
                      <span className="text-sm text-gray-700">
                        {label}
                        {field.unit && <span className="text-gray-400 text-xs ml-1">({field.unit})</span>}
                      </span>
                    </label>
                  )
                })}
              </div>
            </div>
          )
        })
      ) : (
        // No groups: show all fields in a flat wrap (dynamic discovery mode)
        <div className="flex flex-wrap gap-2">
          {fields.map(field => {
            const ch = channelIndexFromKey(field.key)
            const label = ch >= 0 ? resolveLabel(ch, field.key, fields, channelNames) : field.label
            return (
              <label key={field.key} className="inline-flex items-center gap-1.5 cursor-pointer">
                <input
                  type="checkbox"
                  checked={selected.includes(field.key)}
                  onChange={() => toggle(field.key)}
                  className="rounded border-gray-300 text-blue-600 focus:ring-blue-500"
                />
                <span className="text-sm text-gray-700">
                  {label}
                  {field.unit && <span className="text-gray-400 text-xs ml-1">({field.unit})</span>}
                </span>
              </label>
            )
          })}
        </div>
      )}
      {!hasGroups && ungroupedFields.length > 0 && (
        <div className="flex flex-wrap gap-2">
          {ungroupedFields.map(field => {
            const ch = channelIndexFromKey(field.key)
            const label = ch >= 0 ? resolveLabel(ch, field.key, fields, channelNames) : field.label
            return (
              <label key={field.key} className="inline-flex items-center gap-1.5 cursor-pointer">
                <input
                  type="checkbox"
                  checked={selected.includes(field.key)}
                  onChange={() => toggle(field.key)}
                  className="rounded border-gray-300 text-blue-600 focus:ring-blue-500"
                />
                <span className="text-sm text-gray-700">
                  {label}
                  {field.unit && <span className="text-gray-400 text-xs ml-1">({field.unit})</span>}
                </span>
              </label>
            )
          })}
        </div>
      )}
      {hasGroups && ungroupedFields.length > 0 && (
        <div className="flex flex-wrap gap-2">
          {ungroupedFields.map(field => {
            const ch = channelIndexFromKey(field.key)
            const label = ch >= 0 ? resolveLabel(ch, field.key, fields, channelNames) : field.label
            return (
              <label key={field.key} className="inline-flex items-center gap-1.5 cursor-pointer">
                <input
                  type="checkbox"
                  checked={selected.includes(field.key)}
                  onChange={() => toggle(field.key)}
                  className="rounded border-gray-300 text-blue-600 focus:ring-blue-500"
                />
                <span className="text-sm text-gray-700">
                  {label}
                  {field.unit && <span className="text-gray-400 text-xs ml-1">({field.unit})</span>}
                </span>
              </label>
            )
          })}
        </div>
      )}
    </div>
  )
}