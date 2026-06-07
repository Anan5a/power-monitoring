export default function PlaceholderWidget({ label }: { label?: string }) {
  return (
    <div className="h-full w-full rounded-2xl border border-dashed border-slate-200 flex items-center justify-center text-slate-300 text-sm">
      {label ?? 'Placeholder'}
    </div>
  )
}
