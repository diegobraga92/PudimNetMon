import { useMemo, useState } from 'react'
import { BellRing, CheckCheck, ChevronRight } from 'lucide-react'
import type { SeverityFilter } from '../../types'
import { useAckAllAlerts, useAlerts } from '../../hooks/useAlerts'
import { SEVERITY_COLORS } from '../../lib/constants'
import { cn } from '../../lib/cn'
import { useDashboard } from '../../context/DashboardContext'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { useToast } from '../ui/toast'
import { AlertCard } from './AlertCard'

const SEVERITY_TABS: { value: SeverityFilter; label: string; color: string }[] = [
  { value: 'all', label: 'All', color: 'var(--fg-muted)' },
  { value: 'critical', label: 'Critical', color: SEVERITY_COLORS.critical },
  { value: 'warning', label: 'Warning', color: SEVERITY_COLORS.warning },
  { value: 'info', label: 'Info', color: SEVERITY_COLORS.info },
]

export function AlertsPanel({ compact = false }: { compact?: boolean }) {
  const { data, isLoading } = useAlerts()
  const ackAll = useAckAllAlerts()
  const { toast } = useToast()
  const { setView } = useDashboard()
  const [filter, setFilter] = useState<SeverityFilter>('all')

  const alerts = data ?? []

  const counts = useMemo(() => {
    const c: Record<SeverityFilter, number> = { all: alerts.length, critical: 0, warning: 0, info: 0 }
    alerts.forEach((a) => {
      if (a.severity === 'critical') c.critical += 1
      else if (a.severity === 'warning') c.warning += 1
      else c.info += 1
    })
    return c
  }, [alerts])

  const filtered = filter === 'all' ? alerts : alerts.filter((a) => a.severity === filter)
  const unacked = alerts.filter((a) => !a.acknowledged)

  if (isLoading) {
    return (
      <Card className="p-5">
        <ListSkeleton rows={4} />
      </Card>
    )
  }

  if (alerts.length === 0) {
    return (
      <EmptyState
        icon={<BellRing className="size-8" aria-hidden="true" />}
        title="No active alerts"
        description="All checks are within bounds. New alerts will appear here instantly."
      />
    )
  }

  const showGrid = compact ? filtered.slice(0, 6) : filtered

  return (
    <div className="space-y-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="inline-flex h-9 items-center gap-1 rounded-lg bg-surface-muted p-1" role="tablist" aria-label="Filter by severity">
          {SEVERITY_TABS.map((tab) => (
            <button
              key={tab.value}
              role="tab"
              aria-selected={filter === tab.value}
              onClick={() => setFilter(tab.value)}
              className={cn(
                'inline-flex h-7 items-center gap-1.5 rounded-md px-3 text-sm font-medium transition-colors',
                'focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
                filter === tab.value
                  ? 'bg-surface text-fg shadow-sm'
                  : 'text-fg-muted hover:text-fg',
              )}
            >
              <span className="size-1.5 rounded-full" style={{ backgroundColor: tab.color }} aria-hidden="true" />
              {tab.label}
              <span className="text-xs text-fg-subtle">{counts[tab.value]}</span>
            </button>
          ))}
        </div>

        {!compact && unacked.length > 1 && (
          <Button
            variant="secondary"
            size="sm"
            loading={ackAll.isPending}
            onClick={() =>
              ackAll.mutate(alerts, {
                onSuccess: () => {
                  toast({
                    title: 'Alerts acknowledged',
                    description: `${unacked.length} alert${unacked.length > 1 ? 's' : ''} marked as acknowledged.`,
                    variant: 'success',
                  })
                },
                onError: (err) => {
                  toast({
                    title: 'Could not acknowledge all',
                    description: err instanceof Error ? err.message : 'Try again in a moment.',
                    variant: 'error',
                  })
                },
              })
            }
          >
            <CheckCheck className="size-3.5" aria-hidden="true" />
            Acknowledge all ({unacked.length})
          </Button>
        )}
      </div>

      <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
        {showGrid.map((alert) => (
          <AlertCard key={`${alert.rule_id}-${alert.agent_id}-${alert.target}`} alert={alert} />
        ))}
      </div>

      {compact && filtered.length > 6 && (
        <button
          onClick={() => setView('alerts')}
          className="inline-flex items-center gap-1 text-xs font-medium text-accent transition-colors hover:text-accent/80 focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40"
        >
          View all {filtered.length} alerts
          <ChevronRight className="size-3.5" aria-hidden="true" />
        </button>
      )}
    </div>
  )
}
