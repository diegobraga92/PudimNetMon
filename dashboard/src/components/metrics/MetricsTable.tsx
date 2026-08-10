import { useMemo, useState } from 'react'
import { ArrowDown, ArrowUp, ArrowUpDown, Download, Search, Table2 } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useMetrics } from '../../hooks/useMetrics'
import { CHECK_TYPE_LABELS } from '../../lib/constants'
import { downloadCsv } from '../../lib/csv'
import { formatDateTime } from '../../lib/formatters'
import { cn } from '../../lib/cn'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { FilterBar } from '../ui/FilterBar'
import { useToast } from '../ui/toast'

type SortKey = 'time_ms' | 'agent_id' | 'check_type' | 'target' | 'value' | 'success'
type SortDir = 'asc' | 'desc'

const MAX_ROWS = 500

const COLUMNS: { key: SortKey; label: string }[] = [
  { key: 'time_ms', label: 'Time' },
  { key: 'agent_id', label: 'Agent' },
  { key: 'check_type', label: 'Check Type' },
  { key: 'target', label: 'Target' },
  { key: 'value', label: 'Value' },
  { key: 'success', label: 'Status' },
]

export function MetricsTable() {
  const { selectedAgent, selectedCheck, windowSeconds } = useDashboard()
  const metricsQuery = useMetrics({ agentId: selectedAgent, checkType: selectedCheck, windowSeconds })
  const { toast } = useToast()

  const [search, setSearch] = useState('')
  const [sortKey, setSortKey] = useState<SortKey>('time_ms')
  const [sortDir, setSortDir] = useState<SortDir>('desc')

  const metrics = metricsQuery.data ?? []

  const toggleSort = (key: SortKey) => {
    if (key === sortKey) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'))
    } else {
      setSortKey(key)
      setSortDir(key === 'time_ms' ? 'desc' : 'asc')
    }
  }

  const rows = useMemo(() => {
    const query = search.trim().toLowerCase()
    const filtered = query
      ? metrics.filter((m) =>
          [m.agent_id, m.target, m.check_type].some((v) => v.toLowerCase().includes(query)),
        )
      : metrics

    const multiplier = sortDir === 'asc' ? 1 : -1
    const sorted = [...filtered].sort((a, b) => {
      if (sortKey === 'value') return (a.value - b.value) * multiplier
      if (sortKey === 'success') return (Number(a.success) - Number(b.success)) * multiplier
      if (sortKey === 'time_ms') return (a.time_ms - b.time_ms) * multiplier
      return String(a[sortKey]).localeCompare(String(b[sortKey])) * multiplier
    })
    return sorted.slice(0, MAX_ROWS)
  }, [metrics, search, sortKey, sortDir])

  const exportCsv = () => {
    downloadCsv(
      `pudim-metrics-${Date.now()}.csv`,
      ['time', 'agent_id', 'check_type', 'target', 'success', 'value'],
      rows.map((m) => [
        new Date(m.time_ms).toISOString(),
        m.agent_id,
        m.check_type,
        m.target,
        m.success ? 'ok' : 'fail',
        m.value,
      ]),
    )
    toast({ title: 'CSV exported', description: `${rows.length} rows downloaded.`, variant: 'success' })
  }
  return (
    <Card className="overflow-hidden">
      <div className="flex flex-wrap items-center justify-between gap-3 border-b border-border p-4">
        <div>
          <h2 className="text-sm font-semibold text-fg">Raw Metrics</h2>
          <p className="mt-0.5 text-xs text-fg-muted">
            Showing {rows.length.toLocaleString()} of {metrics.length.toLocaleString()} rows in window.
          </p>
        </div>
        <div className="flex flex-wrap items-center gap-2">
          <FilterBar />
          <div className="relative">
            <Search className="pointer-events-none absolute left-2.5 top-1/2 size-3.5 -translate-y-1/2 text-fg-subtle" aria-hidden="true" />
            <input
              type="search"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              placeholder="Filter rows…"
              aria-label="Filter metric rows"
              className="h-9 w-40 rounded-lg border border-border bg-bg pl-8 pr-3 text-sm text-fg placeholder:text-fg-subtle focus:border-accent focus:outline-none focus:ring-2 focus:ring-accent/30"
            />
          </div>
          <Button variant="secondary" size="sm" onClick={exportCsv} disabled={rows.length === 0}>
            <Download className="size-3.5" aria-hidden="true" />
            Export CSV
          </Button>
        </div>
      </div>

      {metricsQuery.isLoading ? (
        <div className="p-4">
          <ListSkeleton rows={8} />
        </div>
      ) : rows.length === 0 ? (
        <div className="p-4">
          <EmptyState
            icon={<Table2 className="size-8" aria-hidden="true" />}
            title={metrics.length === 0 ? 'No metrics in window' : 'No rows match'}
            description={
              metrics.length === 0
                ? 'Start an agent with probes configured and metrics will appear here.'
                : 'Try widening the time window or clearing the search filter.'
            }
          />
        </div>
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-left text-sm" aria-label="Raw metrics">


            <thead>
              <tr className="border-b border-border text-xs uppercase tracking-wide text-fg-muted">
                {COLUMNS.map((col) => (
                  <th key={col.key} className="whitespace-nowrap px-4 py-2.5 font-medium">
                    <button
                      onClick={() => toggleSort(col.key)}
                      className={cn(
                        'inline-flex items-center gap-1 transition-colors hover:text-fg focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
                        sortKey === col.key && 'text-fg',
                      )}
                    >
                      {col.label}
                      {sortKey === col.key ? (
                        sortDir === 'asc' ? (
                          <ArrowUp className="size-3" aria-hidden="true" />
                        ) : (
                          <ArrowDown className="size-3" aria-hidden="true" />
                        )
                      ) : (
                        <ArrowUpDown className="size-3 text-fg-subtle" aria-hidden="true" />
                      )}
                    </button>
                  </th>
                ))}
              </tr>
            </thead>
            <tbody className="divide-y divide-border">
              {rows.map((m, idx) => (
                <tr
                  key={`${m.time_ms}-${m.agent_id}-${m.check_type}-${m.target}-${idx}`}
                  className="transition-colors hover:bg-surface-muted"
                >
                  <td className="whitespace-nowrap px-4 py-2 text-fg-muted" title={formatDateTime(m.time_ms)}>
                    {formatDateTime(m.time_ms)}
                  </td>
                  <td className="whitespace-nowrap px-4 py-2 font-mono text-xs text-fg">{m.agent_id}</td>
                  <td className="whitespace-nowrap px-4 py-2 text-fg-muted">
                    {CHECK_TYPE_LABELS[m.check_type] ?? m.check_type}
                  </td>
                  <td className="max-w-56 truncate px-4 py-2 text-fg-muted" title={m.target}>
                    {m.target}
                  </td>
                  <td
                    className={cn(
                      'whitespace-nowrap px-4 py-2 font-mono text-xs',
                      m.success ? 'text-fg' : 'text-critical',
                    )}
                  >
                    {m.value}
                  </td>
                  <td className="px-4 py-2">
                    <span
                      className={cn(
                        'inline-flex items-center rounded-full px-2 py-0.5 text-[11px] font-medium',
                        m.success ? 'bg-success/15 text-success' : 'bg-critical/15 text-critical',
                      )}
                    >
                      {m.success ? 'OK' : 'FAIL'}
                    </span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {metrics.length > MAX_ROWS && (
        <p className="border-t border-border px-4 py-2.5 text-xs text-fg-subtle">
          Limited to the most recent {MAX_ROWS.toLocaleString()} rows. Export CSV for the full set.
        </p>
      )}
    </Card>
  )
}
