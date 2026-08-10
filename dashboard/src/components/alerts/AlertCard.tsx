import { AlertCircle, AlertTriangle, BellOff, Check, Info } from 'lucide-react'
import type { ActiveAlert } from '../../types'
import { useAckAlert } from '../../hooks/useAlerts'
import { formatRelativeTime } from '../../lib/formatters'
import { SEVERITY_COLORS } from '../../lib/constants'
import { cn } from '../../lib/cn'
import { Badge } from '../ui/Badge'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { useToast } from '../ui/toast'

function severityBadgeVariant(severity: string) {
  if (severity === 'critical') return 'critical' as const
  if (severity === 'warning') return 'warning' as const
  return 'info' as const
}

function SeverityIcon({ severity }: { severity: string }) {
  const className = 'size-4 shrink-0'
  if (severity === 'critical') return <AlertTriangle className={className} aria-hidden="true" />
  if (severity === 'warning') return <AlertCircle className={className} aria-hidden="true" />
  return <Info className={className} aria-hidden="true" />
}

export function AlertCard({ alert }: { alert: ActiveAlert }) {
  const ack = useAckAlert()
  const { toast } = useToast()
  const color = SEVERITY_COLORS[alert.severity] ?? '#f9ca24'

  const handleAck = () => {
    ack.mutate(alert, {
      onSuccess: () => {
        toast({
          title: 'Alert acknowledged',
          description: `${alert.rule_name} on ${alert.agent_id}${alert.target ? ` → ${alert.target}` : ''}`,
          variant: 'success',
        })
      },
      onError: (err) => {
        toast({
          title: 'Could not acknowledge',
          description: err instanceof Error ? err.message : 'Try again in a moment.',
          variant: 'error',
        })
      },
    })
  }

  return (
    <Card
      className={cn(
        'flex flex-col gap-2 p-4 transition-opacity',
        alert.acknowledged && 'opacity-70',
        !alert.acknowledged && alert.severity === 'critical' && 'animate-alert-pulse',
      )}
    >
      <div className="flex items-start justify-between gap-2">
        <div className="flex min-w-0 items-center gap-2">
          <span style={{ color }} className="shrink-0">
            <SeverityIcon severity={alert.severity} />
          </span>
          <strong className="truncate text-sm text-fg">{alert.rule_name}</strong>
        </div>
        <Badge variant={severityBadgeVariant(alert.severity)}>{alert.severity}</Badge>
      </div>

      <p className="truncate text-xs text-fg-muted" title={`${alert.agent_id} → ${alert.target}`}>
        <span className="font-medium text-fg">{alert.agent_id}</span>
        <span className="mx-1 text-fg-subtle">→</span>
        {alert.target || 'all targets'}
      </p>

      <p className="text-xs text-fg-muted">
        value <span className="font-semibold text-fg">{alert.value}</span>
        <span className="text-fg-subtle"> / threshold {alert.threshold}</span>
      </p>

      <div className="mt-auto flex items-center justify-between gap-2 border-t border-border pt-2">
        <span className="text-[11px] text-fg-subtle" title={new Date(alert.fired_ms).toLocaleString()}>
          fired {formatRelativeTime(alert.fired_ms)}
        </span>
        {alert.acknowledged ? (
          <Badge variant="success">
            <Check className="size-3" aria-hidden="true" />
            Acknowledged
          </Badge>
        ) : (
          <Button
            variant="secondary"
            size="sm"
            loading={ack.isPending}
            onClick={handleAck}
            disabled={ack.isPending}
          >
            <BellOff className="size-3.5" aria-hidden="true" />
            Ack
          </Button>
        )}
      </div>
    </Card>
  )
}
