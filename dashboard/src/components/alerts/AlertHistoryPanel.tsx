import { useState } from 'react'
import { History } from 'lucide-react'
import type { AlertHistoryEntry } from '../../types'
import { useAlertHistory } from '../../hooks/useAlertHistory'
import { formatRelativeTime } from '../../lib/formatters'
import { cn } from '../../lib/cn'
import { Badge } from '../ui/Badge'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'

type StatusFilter = 'all' | 'firing' | 'resolved'

function severityVariant(severity: string) {
  if (severity === 'critical') return 'critical' as const
  if (severity === 'warning') return 'warning' as const
  return 'info' as const
}

function HistoryRow({ entry }: { entry: AlertHistoryEntry }) {
  return (
    <div className="flex items-start gap-3 px-4 py-3 transition-colors hover:bg-surface-muted">
      <span
        className="mt-1.5 size-2 shrink-0 rounded-full"
        style={{ backgroundColor: entry.status === 'firing' ? '#ff6b6b' : '#4ecdc4' }}
        aria-hidden="true"
      />
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-center gap-2">
          <strong className="text-sm text-fg">{entry.rule_name}</strong>
          <Badge variant={severityVariant(entry.severity)}>{entry.severity}</Badge>
          <Badge variant={entry.status === 'firing' ? 'critical' : 'success'}>
            {entry.status === 'firing' ? 'FIRING' : 'RESOLVED'}
          </Badge>
        </div>
        <p className="mt-0.5 text-xs text-fg-muted">
          {entry.agent_id} <span className="text-fg-subtle">→</span> {entry.target || 'all targets'} · {entry.check_type}
        </p>
        <p className="text-xs text-fg-subtle">
          value <span className="font-medium text-fg">{entry.value}</span>
          <span className="text-fg-subtle"> (threshold {entry.threshold})</span>
          {entry.detail ? ` · ${entry.detail}` : ''}
        </p>
      </div>
      <span className="shrink-0 text-xs text-fg-subtle" title={new Date(entry.time_ms).toLocaleString()}>
        {formatRelativeTime(entry.time_ms)}
      </span>
    </div>
  )
}

export function AlertHistoryPanel({ compact = false }: { compact?: boolean }) {
  const { data, isLoading } = useAlertHistory(50)
  const [filter, setFilter] = useState<StatusFilter>('all')
  const entries = data ?? []

  const filtered = filter === 'all' ? entries : entries.filter((e) => e.status === filter)
  const shown = compact ? filtered.slice(0, 8) : filtered

  const tabs: { value: StatusFilter; label: string; count: number }[] = [
    { value: 'all', label: 'All', count: entries.length },
    { value: 'firing', label: 'Firing', count: entries.filter((e) => e.status === 'firing').length },
    { value: 'resolved', label: 'Resolved', count: entries.filter((e) => e.status === 'resolved').length },
  ]

  return (
    <Card className="overflow-hidden">
      <div className="flex flex-wrap items-center justify-between gap-2 border-b border-border px-4 py-3">
        {!compact && (
          <div className="inline-flex h-7 items-center gap-1 rounded-lg bg-surface-muted p-1" role="tablist" aria-label="Filter history by status">
            {tabs.map((tab) => (
              <button
                key={tab.value}
                role="tab"
                aria-selected={filter === tab.value}
                onClick={() => setFilter(tab.value)}
                className={cn(
                  'inline-flex h-6 items-center gap-1 rounded-md px-2.5 text-xs font-medium transition-colors',
                  filter === tab.value ? 'bg-surface text-fg shadow-sm' : 'text-fg-muted hover:text-fg',
                )}
              >
                {tab.label}
                <span className="text-[10px] text-fg-subtle">{tab.count}</span>
              </button>
            ))}
          </div>
        )}
        <span className="text-xs text-fg-subtle">{entries.length} events</span>
      </div>

      {isLoading ? (
        <div className="p-4">
          <ListSkeleton rows={6} />
        </div>
      ) : shown.length === 0 ? (
        <div className="p-4">
          <EmptyState
            icon={<History className="size-8" aria-hidden="true" />}
            title="No alert events yet"
            description="Fired and resolved alerts will be listed here over time."
          />
        </div>
      ) : (
        <ul className="divide-y divide-border">
          {shown.map((entry, idx) => (
            <HistoryRow key={`${entry.time_ms}-${idx}`} entry={entry} />
          ))}
        </ul>
      )}
    </Card>
  )
}
