import { MetricsTable } from '../metrics/MetricsTable'

export function MetricsPage() {
  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Metrics Explorer</h1>
        <p className="text-base text-fg-muted">
          Inspect raw probe results, sort and filter, or export to CSV.
        </p>
      </div>
      <MetricsTable />
    </div>
  )
}
