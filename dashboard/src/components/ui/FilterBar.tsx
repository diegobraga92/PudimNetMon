import { RotateCcw } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useAgents } from '../../hooks/useAgents'
import { CHECK_TYPE_OPTIONS, TIME_WINDOWS } from '../../lib/constants'
import { cn } from '../../lib/cn'
import { Button } from '../ui/Button'
import { Select } from '../ui/Select'

/**
 * Shared agent / check-type / time-window filter toolbar.
 * Reads and writes the dashboard-level filter state so the chart, table and
 * overview stay in sync.
 */
export function FilterBar({ className }: { className?: string }) {
  const {
    selectedAgent,
    setSelectedAgent,
    selectedCheck,
    setSelectedCheck,
    windowSeconds,
    setWindowSeconds,
  } = useDashboard()
  const agentsQuery = useAgents()

  const agents = agentsQuery.data?.agents ?? []
  const agentOptions = [
    { value: 'all', label: 'All Agents' },
    ...agents.map((a) => ({ value: a.agent_id, label: a.agent_id })),
  ]

  const hasActiveFilters = selectedAgent !== 'all' || selectedCheck !== 'all' || windowSeconds !== 300

  const reset = () => {
    setSelectedAgent('all')
    setSelectedCheck('all')
    setWindowSeconds(300)
  }

  return (
    <div className={cn('flex flex-wrap items-center gap-2', className)}>
      <Select
        ariaLabel="Filter by agent"
        value={selectedAgent}
        onValueChange={setSelectedAgent}
        options={agentOptions}
        className="w-44"
      />
      <Select
        ariaLabel="Filter by check type"
        value={selectedCheck}
        onValueChange={(v) => setSelectedCheck(v as typeof selectedCheck)}
        options={CHECK_TYPE_OPTIONS}
        className="w-56"
      />
      <Select
        ariaLabel="Time window"
        value={String(windowSeconds)}
        onValueChange={(v) => setWindowSeconds(Number(v))}
        options={TIME_WINDOWS.map((w) => ({ value: String(w.value), label: w.label }))}
        className="w-28"
      />
      {hasActiveFilters && (
        <Button variant="ghost" size="sm" onClick={reset} aria-label="Reset filters">
          <RotateCcw className="size-3.5" aria-hidden="true" />
          Reset
        </Button>
      )}
    </div>
  )
}
