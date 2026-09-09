import { useState } from 'react'
import {
  CalendarClock,
  ChevronDown,
  LineChart as LineChartIcon,
  Plus,
  TerminalSquare,
  Trash2,
} from 'lucide-react'
import type { CommandSchedule } from '../../types'
import { useAgents } from '../../hooks/useAgents'
import {
  useCommandSchedules,
  useDeleteSchedule,
  useSetScheduleEnabled,
} from '../../hooks/useCommandSchedules'
import { cn } from '../../lib/cn'
import { formatCountdown, formatDuration } from '../../lib/formatters'
import { Badge, type BadgeVariant } from '../ui/Badge'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { EmptyState } from '../ui/EmptyState'
import { ListSkeleton } from '../ui/LoadingSkeleton'
import { Switch } from '../ui/Switch'
import { useToast } from '../ui/toast'
import { NewScheduleDialog } from './NewScheduleDialog'
import { ScheduleHistory } from './ScheduleHistory'

function stateBadge(s: CommandSchedule): { label: string; variant: BadgeVariant } {
  if (s.expired) return { label: 'Expired', variant: 'neutral' }
  if (!s.enabled) return { label: 'Paused', variant: 'neutral' }
  if (s.active) return { label: 'Active', variant: 'success' }
  return { label: 'Waiting', variant: 'info' }
}

export function SchedulesPanel() {
  const { toast } = useToast()
  const agents = useAgents()
  const schedules = useCommandSchedules()
  const setEnabled = useSetScheduleEnabled()
  const remove = useDeleteSchedule()
  const [dialogOpen, setDialogOpen] = useState(false)
  const [confirmDeleteId, setConfirmDeleteId] = useState<string | null>(null)
  const [expandedId, setExpandedId] = useState<string | null>(null)

  const items = schedules.data?.schedules ?? []
  const knownAgents = agents.data?.agents ?? []

  const onDelete = (s: CommandSchedule) => {
    if (confirmDeleteId !== s.id) {
      setConfirmDeleteId(s.id)
      window.setTimeout(() => setConfirmDeleteId(null), 3000)
      return
    }
    remove.mutate(s.id, {
      onSuccess: (data) => {
        setConfirmDeleteId(null)
        toast({
          title: data.success === false ? 'Could not delete schedule' : 'Schedule deleted',
          description:
            data.success === false
              ? data.error || 'Unknown error'
              : `${s.command_id} on ${s.agent_id}`,
          variant: data.success === false ? 'error' : 'success',
        })
      },
      onError: (err) => {
        toast({
          title: 'Could not delete schedule',
          description: err instanceof Error ? err.message : 'Unknown error',
          variant: 'error',
        })
      },
    })
  }

  if (schedules.isLoading) {
    return (
      <Card className="p-5">
        <ListSkeleton rows={4} />
      </Card>
    )
  }

  return (
    <div className="space-y-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="text-sm text-fg-muted">
          {items.length === 0
            ? 'Nothing scheduled yet.'
            : `${items.length} schedule${items.length === 1 ? '' : 's'} defined.`}
        </p>
        <Button
          size="sm"
          onClick={() => setDialogOpen(true)}
          disabled={knownAgents.length === 0}
        >
          <Plus className="size-3.5" aria-hidden="true" />
          New schedule
        </Button>
      </div>
      {items.length === 0 ? (
        <EmptyState
          icon={<CalendarClock className="size-8" aria-hidden="true" />}
          title="No scheduled commands"
          description="Schedule heavy maintenance commands — speedtests, sustained pings, route-quality probes — to run on selected agents at a chosen frequency inside a time window."
          action={
            knownAgents.length > 0 ? (
              <Button size="sm" onClick={() => setDialogOpen(true)}>
                <Plus className="size-3.5" aria-hidden="true" />
                Schedule the first command
              </Button>
            ) : undefined
          }
        />
      ) : (
        <div className="space-y-2">
          {items.map((s) => {
            const state = stateBadge(s)
            const params = Object.entries(s.params ?? {})
            return (
              <Card key={s.id} className="p-4">
                <div className="flex flex-wrap items-start justify-between gap-3">
                  <div className="min-w-0">
                    <div className="flex flex-wrap items-center gap-2">
                      <TerminalSquare className="size-4 text-fg-muted" aria-hidden="true" />
                      <span className="font-mono text-sm font-medium text-fg">{s.command_id}</span>
                      <Badge variant={state.variant}>{state.label}</Badge>
                      {s.label && <span className="text-xs text-fg-muted">{s.label}</span>}
                    </div>
                    <dl className="mt-1.5 grid grid-cols-2 gap-x-6 gap-y-0.5 text-xs sm:grid-cols-4">
                      <div>
                        <dt className="text-fg-subtle">Agent</dt>
                        <dd className="truncate font-mono text-fg-muted" title={s.agent_id}>
                          {s.agent_id}
                        </dd>
                      </div>
                      <div>
                        <dt className="text-fg-subtle">Every</dt>
                        <dd className="text-fg-muted">{formatDuration(s.interval_sec)}</dd>
                      </div>
                      <div>
                        <dt className="text-fg-subtle">Ends</dt>
                        <dd className="text-fg-muted">
                          {s.expired ? 'expired' : formatCountdown(s.window_end_unix_ms)}
                        </dd>
                      </div>
                      <div>
                        <dt className="text-fg-subtle">Next run</dt>
                        <dd className="text-fg-muted">
                          {s.expired ? '—' : s.enabled ? formatCountdown(s.next_run_unix_ms) : 'paused'}
                        </dd>
                      </div>
                    </dl>
                    {params.length > 0 && (
                      <p
                        className="mt-1.5 truncate font-mono text-[11px] text-fg-subtle"
                        title={JSON.stringify(s.params)}
                      >
                        {params.map(([k, v]) => `${k}=${v}`).join('  ')}
                      </p>
                    )}
                  </div>

                  <div className="flex shrink-0 flex-wrap items-center gap-3">
                    <Button
                      variant={expandedId === s.id ? 'secondary' : 'outline'}
                      size="sm"
                      onClick={() => setExpandedId((cur) => (cur === s.id ? null : s.id))}
                      aria-expanded={expandedId === s.id}
                    >
                      <LineChartIcon className="size-3.5" aria-hidden="true" />
                      Trend & history
                      <ChevronDown
                        className={cn('size-3 transition-transform', expandedId === s.id && 'rotate-180')}
                        aria-hidden="true"
                      />
                    </Button>
                    <Switch
                      checked={s.enabled}
                      disabled={s.expired}
                      label={s.enabled ? 'Enabled' : 'Paused'}
                      onCheckedChange={(enabled) =>
                        setEnabled.mutate({ id: s.id, enabled })
                      }
                    />
                    <Button
                      variant="danger"
                      size="sm"
                      loading={remove.isPending && confirmDeleteId === s.id}
                      onClick={() => onDelete(s)}
                    >
                      <Trash2 className="size-3.5" aria-hidden="true" />
                      {confirmDeleteId === s.id ? 'Confirm?' : 'Delete'}
                    </Button>
                  </div>
                </div>

                {expandedId === s.id && (
                  <div className="mt-3 border-t border-border pt-3">
                    <ScheduleHistory
                      scheduleId={s.id}
                      commandId={s.command_id}
                      agentId={s.agent_id}
                    />
                  </div>
                )}
              </Card>
            )
          })}
        </div>
      )}

      <NewScheduleDialog
        open={dialogOpen}
        onOpenChange={setDialogOpen}
        agents={knownAgents}
      />
    </div>
  )
}
