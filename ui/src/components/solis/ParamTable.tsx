interface Column {
  key: string
  label: string
}

interface ParamTableProps {
  title: string
  columns: Column[]
  rows: Record<string, string | number | null>[]
}

export default function ParamTable({ title, columns, rows }: ParamTableProps) {
  return (
    <div className="bg-white rounded-lg border border-gray-200 shadow-sm overflow-hidden">
      <div className="px-4 py-3 border-b border-gray-200 font-semibold text-gray-700 text-sm">{title}</div>
      <div className="overflow-x-auto">
        <table className="w-full text-sm">
          <thead className="bg-gray-50">
            <tr>
              {columns.map((c) => (
                <th key={c.key} className="px-4 py-2 text-left text-gray-500 font-medium">{c.label}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, i) => (
              <tr key={i} className="border-t border-gray-100">
                {columns.map((c) => (
                  <td key={c.key} className="px-4 py-2 text-gray-700">{row[c.key] ?? '--'}</td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}