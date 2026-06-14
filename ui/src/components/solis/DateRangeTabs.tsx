type Range = 'day' | 'month' | 'year' | 'total'

interface DateRangeTabsProps {
  value: Range
  onChange: (range: Range) => void
}

const options: { value: Range; label: string }[] = [
  { value: 'day', label: 'Day' },
  { value: 'month', label: 'Month' },
  { value: 'year', label: 'Year' },
  { value: 'total', label: 'Total' },
]

export default function DateRangeTabs({ value, onChange }: DateRangeTabsProps) {
  return (
    <div className="inline-flex rounded-md border border-gray-300 overflow-hidden bg-white">
      {options.map((opt) => (
        <button
          key={opt.value}
          type="button"
          onClick={() => onChange(opt.value)}
          className={`px-4 py-1.5 text-sm font-medium border-r border-gray-200 last:border-r-0 ${
            value === opt.value ? 'bg-[#3c4454] text-white' : 'text-gray-600 hover:bg-gray-50'
          }`}
        >
          {opt.label}
        </button>
      ))}
    </div>
  )
}
