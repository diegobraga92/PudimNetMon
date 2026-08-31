import { Beaker, Crosshair, TerminalSquare } from 'lucide-react'
import type { AgentInfo } from '../../types'
import { useDashboard } from '../../context/DashboardContext'
import { formatRelativeTime } from '../../lib/formatters'
import { cn } from '../../lib/cn'
import { Badge } from '../ui/Badge'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { Tooltip } from '../ui/Tooltip'
import { SparklineChart } from '../charts/MetricsChart'

interface AgentCardProps {
  agent: AgentInfo
  sparkline?: { time_ms: number; value: number }[]
  onRunDiagnostic?: (agent: AgentInfo) => void
  onRunCommand?: (agent: AgentInfo) => void
  diagnosticRunning?: boolean
}

export function AgentCard({ agent, sparkline, onRunDiagnostic, onRunCommand, diagnosticRunning }: AgentCardProps) {
  const { setSelectedAgent } = useDashboard()

  return (
    <Card className="flex flex-col gap-3 p-4 transition-colors hover:border-border-strong">
      <div className="flex items-start justify-between gap-2">
        <div className="flex min-w-0 items-center gap-2">
          <span
            className={cn('mt-0.5 size-2 shrink-0 rounded-full', agent.alive ? 'bg-success' : 'bg-critical')}
            aria-hidden="true"
          />
          <strong className="truncate font-mono text-sm text-fg" title={agent.agent_id}>
            {agent.agent_id}
          </strong>
        </div>
        <Badge variant={agent.alive ? 'success' : 'critical'}>{agent.alive ? 'Alive' : 'Offline'}</Badge>
      </div>

      {sparkline && sparkline.length > 1 ? (
        <div className="rounded-lg bg-surface-muted p-2">
          <SparklineChart data={sparkline} color={agent.alive ? '#3fb950' : '#ff6b6b'} />
        </div>
      ) : (
        <div
          className="sparkline-placeholder rounded-lg bg-surface-muted p-2"
          style={{ height: 40 }}
          title="No probe data yet"
          aria-hidden="true"
        />
      )}

      <dl className="grid grid-cols-2 gap-x-3 gap-y-1 text-xs">
        <dt className="text-fg-subtle">Version</dt>
        <dd className="truncate text-right font-mono text-fg-muted" title={agent.version}>{agent.version}</dd>
        <dt className="text-fg-subtle">Interval</dt>
        <dd className="text-right text-fg-muted">{agent.interval_ms} ms</dd>
        <dt className="text-fg-subtle">Last seen</dt>
        <dd className="text-right text-fg-muted">{formatRelativeTime(agent.last_seen_unix_ms)}</dd>
        <dt className="text-fg-subtle">First seen</dt>
        <dd className="text-right text-fg-muted">{formatRelativeTime(agent.first_seen_unix_ms)}</dd>
      </dl>

      <div className="mt-auto flex items-center gap-2 border-t border-border pt-3">
        <Tooltip content="Focus the overview charts on this agent">
          <Button variant="secondary" size="sm" onClick={() => setSelectedAgent(agent.agent_id)}>
            <Crosshair className="size-3.5" aria-hidden="true" />
            Focus agent
          </Button>
        </Tooltip>
        {agent.diagnostic_endpoint && onRunDiagnostic && (
          <Button
            variant="outline"
            size="sm"
            loading={diagnosticRunning}
            onClick={() => onRunDiagnostic(agent)}
          >
            <Beaker className="size-3.5" aria-hidden="true" />
            Diagnose
          </Button>
        )}
        {agent.diagnostic_endpoint && onRunCommand && (
          <Button variant="outline" size="sm" onClick={() => onRunCommand(agent)}>
            <TerminalSquare className="size-3.5" aria-hidden="true" />
            Commands
          </Button>
        )}
      </div>
    </Card>
  )
}
