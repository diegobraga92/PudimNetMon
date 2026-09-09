import { AlertTriangle, CheckCircle2, History, Loader2 } from 'lucide-react'
import type { CommandRun } from '../../types'
import { useCommandRuns } from '../../hooks/useCommandSchedules'
import { formatDateTime } from '../../lib/formatters'
import { Badge } from '../ui/Badge'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'

/** Latest executions of scheduled + ad-hoc commands across all agents. */
export function RunHistoryPanel() {
  const runs = useCommandRuns(30)
  const items = runs.data?.runs ?? []

  if (runs.isLoading) {
    return (
      <Card className="p-5">
        <ListSkeleton rows={3} />
      </Card>
    )
  }

  if (items.length === 0) {
    return (
      <EmptyState
        icon={<History className="size-8" aria-hidden="true" />}
        title="No command runs yet"
        description="Every scheduled or manually-run command lands here with its structured result."
      />
    )
  }

  return (
    <div className="space-y-2">
      {items.map((run) => (
        <RunRow key={run.run_id} run={run} />
      ))}
    </div>
  )
}

function statusBadge(run: CommandRun) {
  if (run.running) return <Badge variant="info">Running</Badge>
  if (run.success) return <Badge variant="success">Success</Badge>
  return <Badge variant="critical">Failed</Badge>
}

function RunRow({ run }: { run: CommandRun }) {
  const fields = Object.entries(run.fields ?? {})
  return (
    <Card className="p-3">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <div className="flex min-w-0 items-center gap-2">
          {run.running ? (
            <Loader2 className="size-3.5 shrink-0 animate-spin text-info" aria-hidden="true" />
          ) : run.success ? (
            <CheckCircle2 className="size-3.5 shrink-0 text-success" aria-hidden="true" />
          ) : (
            <AlertTriangle className="size-3.5 shrink-0 text-critical" aria-hidden="true" />
          )}
          <span className="truncate font-mono text-xs font-medium text-fg">{run.command_id}</span>
          <span className="truncate font-mono text-xs text-fg-muted" title={run.agent_id}>
            {run.agent_id}
          </span>
          {run.schedule_id && (
            <span className="hidden truncate font-mono text-[11px] text-fg-subtle sm:inline" title={run.schedule_id}>
              {run.schedule_id}
            </span>
          )}
        </div>
        <div className="flex shrink-0 items-center gap-2">
          <span className="text-[11px] text-fg-subtle">{formatDateTime(run.started_unix_ms)}</span>
          {statusBadge(run)}
        </div>
      </div>

      {run.summary && <p className="mt-1 truncate text-xs text-fg-muted" title={run.summary}>{run.summary}</p>}
      {run.error && <p className="mt-1 truncate text-xs text-critical" title={run.error}>{run.error}</p>}

      {fields.length > 0 && (
        <div className="mt-2 flex flex-wrap gap-x-4 gap-y-0.5">
          {fields.map(([k, v]) => (
            <span key={k} className="font-mono text-[11px] text-fg-subtle">
              {k}=<span className="text-fg-muted">{v}</span>
            </span>
          ))}
        </div>
      )}
    </Card>
  )
}
