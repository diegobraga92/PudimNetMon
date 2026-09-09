import { History } from 'lucide-react'
import { useCommandRuns } from '../../hooks/useCommandSchedules'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { RunCard } from './RunCard'

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
        description="Every scheduled or manually-run command lands here with its structured result. Click a run to see issues and the raw tool output."
      />
    )
  }

  return (
    <div className="space-y-2">
      {items.map((run) => (
        <RunCard key={run.run_id} run={run} />
      ))}
    </div>
  )
}
