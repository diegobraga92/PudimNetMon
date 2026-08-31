import { useMemo, useState } from 'react'
import { Download, Radio } from 'lucide-react'
import type { AgentInfo } from '../../types'
import { useAgents } from '../../hooks/useAgents'
import { useMetrics } from '../../hooks/useMetrics'
import { useDashboard } from '../../context/DashboardContext'
import { cn } from '../../lib/cn'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { AgentCard } from './AgentCard'

type StatusFilter = 'all' | 'alive' | 'offline'

export function AgentsPanel({
  onRunDiagnostic,
  onRunCommand,
}: {
  onRunDiagnostic?: (agent: AgentInfo) => void
  onRunCommand?: (agent: AgentInfo) => void
}) {
  const { data, isLoading } = useAgents()
  const metrics = useMetrics({ agentId: 'all', checkType: 'all', windowSeconds: 300 })
  const { setView } = useDashboard()
  const [filter, setFilter] = useState<StatusFilter>('all')
  const [search, setSearch] = useState('')

  const agents = data?.agents ?? []

  /** One shared metrics query powers every card's sparkline. */
  const sparklines = useMemo(() => {
    const perAgent = new Map<string, { time_ms: number; value: number }[]>()
    for (const m of metrics.data ?? []) {
      if (!m.success) continue
      const arr = perAgent.get(m.agent_id) ?? []
      arr.push({ time_ms: m.time_ms, value: m.value })
      perAgent.set(m.agent_id, arr)
    }
    const map = new Map<string, { time_ms: number; value: number }[]>()
    for (const [agentId, arr] of perAgent) {
      map.set(agentId, arr.sort((a, b) => a.time_ms - b.time_ms).slice(-24))
    }
    return map
  }, [metrics.data])

  const filtered = agents.filter((a) => {
    if (filter === 'alive' && !a.alive) return false
    if (filter === 'offline' && a.alive) return false
    if (search && !a.agent_id.toLowerCase().includes(search.toLowerCase())) return false
    return true
  })

  const aliveCount = agents.filter((a) => a.alive).length

  if (isLoading) {
    return (
      <Card className="p-5">
        <ListSkeleton rows={6} />
      </Card>
    )
  }

  if (agents.length === 0) {
    return (
      <EmptyState
        icon={<Radio className="size-8" aria-hidden="true" />}
        title="No agents connected"
        description="Start a pudim-agent daemon and it will appear here within a heartbeat interval."
        action={
          <Button variant="primary" size="sm" onClick={() => setView('deploy')}>
            <Download className="size-4" aria-hidden="true" />
            Deploy an agent
          </Button>
        }
      />
    )
  }

  const tabs: { value: StatusFilter; label: string; count: number }[] = [
    { value: 'all', label: 'All', count: agents.length },
    { value: 'alive', label: 'Alive', count: aliveCount },
    { value: 'offline', label: 'Offline', count: agents.length - aliveCount },
  ]

  return (
    <div className="space-y-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div className="inline-flex h-9 items-center gap-1 rounded-lg bg-surface-muted p-1" role="tablist" aria-label="Filter agents by status">
          {tabs.map((tab) => (
            <button
              key={tab.value}
              role="tab"
              aria-selected={filter === tab.value}
              onClick={() => setFilter(tab.value)}
              className={cn(
                'inline-flex h-7 items-center gap-1.5 rounded-md px-3 text-sm font-medium transition-colors',
                'focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
                filter === tab.value ? 'bg-surface text-fg shadow-sm' : 'text-fg-muted hover:text-fg',
              )}
            >
              {tab.label}
              <span className="text-xs text-fg-subtle">{tab.count}</span>
            </button>
          ))}
        </div>

        <input
          type="search"
          value={search}
          onChange={(e) => setSearch(e.target.value)}
          placeholder="Search agents…"
          aria-label="Search agents"
          className="h-9 w-full max-w-56 rounded-lg border border-border bg-surface px-3 text-sm text-fg placeholder:text-fg-subtle focus:border-accent focus:outline-none focus:ring-2 focus:ring-accent/30"
        />
      </div>

      {filtered.length === 0 ? (
        <EmptyState title="No agents match" description="Try a different search term or status filter." />
      ) : (
        <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
          {filtered.map((agent) => (
            <AgentCard
              key={agent.agent_id}
              agent={agent}
              sparkline={sparklines.get(agent.agent_id)}
              onRunDiagnostic={onRunDiagnostic}
              onRunCommand={onRunCommand}
            />
          ))}
        </div>
      )}
    </div>
  )
}
