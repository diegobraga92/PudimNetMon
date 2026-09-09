import { useCommandRuns } from '../../hooks/useCommandSchedules'
import { ChartSkeleton } from '../ui/LoadingSkeleton'
import { RunCard } from './RunCard'
import { ScheduleTrendChart } from './ScheduleTrendChart'

interface ScheduleHistoryProps {
  scheduleId: string
  commandId: string
  agentId: string
}

/**
 * Trend plot + recent executions for a single schedule. Mounted lazily when a
 * schedule card is expanded so inactive panels don't poll the collector.
 */
export function ScheduleHistory({ scheduleId, commandId, agentId }: ScheduleHistoryProps) {
  const runs = useCommandRuns(100, scheduleId)
  const items = runs.data?.runs ?? []
  const successful = items.filter((r) => !r.running && r.success).length

  if (runs.isLoading) {
    return (
      <div className="py-2">
        <ChartSkeleton height={140} />
      </div>
    )
  }

  return (
    <div className="space-y-3">
      <div>
        <p className="mb-2 text-xs font-medium text-fg-muted">
          Trend of <span className="font-mono">{commandId}</span> on {agentId}
        </p>
        <ScheduleTrendChart commandId={commandId} runs={items} />
      </div>

      {items.length === 0 ? (
        <p className="rounded-lg border border-border bg-bg px-3 py-4 text-xs text-fg-muted">
          No runs yet for this schedule.
        </p>
      ) : (
        <div className="space-y-2">
          <p className="text-[11px] font-medium uppercase tracking-wide text-fg-subtle">
            {items.length} run{items.length === 1 ? '' : 's'} · {successful} successful · click a
            run for issues and raw output
          </p>
          {items.map((run) => (
            <RunCard key={run.run_id} run={run} />
          ))}
        </div>
      )}
    </div>
  )
}
