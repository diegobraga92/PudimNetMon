import { useMemo, useState } from 'react'
import { CalendarClock, Loader2 } from 'lucide-react'
import type { AgentInfo } from '../../types'
import { useAgentCommands } from '../../hooks/useAgentCommands'
import { useCreateSchedule } from '../../hooks/useCommandSchedules'
import { Button } from '../ui/Button'
import { Dialog } from '../ui/Dialog'
import { Input } from '../ui/Input'
import { Select, type SelectOption } from '../ui/Select'
import { useToast } from '../ui/toast'

const FREQUENCY_OPTIONS: SelectOption[] = [
  { value: '900', label: 'Every 15 minutes' },
  { value: '1800', label: 'Every 30 minutes' },
  { value: '3600', label: 'Every hour' },
  { value: '10800', label: 'Every 3 hours' },
  { value: '21600', label: 'Every 6 hours' },
  { value: '43200', label: 'Every 12 hours' },
  { value: '86400', label: 'Every day' },
]

const WINDOW_OPTIONS: SelectOption[] = [
  { value: '21600', label: 'Next 6 hours' },
  { value: '43200', label: 'Next 12 hours' },
  { value: '86400', label: 'Next 24 hours' },
  { value: '259200', label: 'Next 3 days' },
  { value: '604800', label: 'Next 7 days' },
]

const DEFAULT_PARAM_HINTS: Record<string, string> = {
  target: 'Hostname or IP to test against',
  count: 'Packets to send (default 100)',
  interval_ms: 'Delay between packets (default 200 ms)',
  packets: 'Probes per hop (default 20)',
  server_id: 'Optional speedtest server id',
}

interface NewScheduleDialogProps {
  open: boolean
  onOpenChange: (open: boolean) => void
  agents: AgentInfo[]
}

/**
 * Dialog that collects everything needed to schedule a heavy command on one
 * agent: which command, how often, and for how long.
 */
export function NewScheduleDialog({
  open,
  onOpenChange,
  agents,
}: NewScheduleDialogProps) {
  const { toast } = useToast()
  const create = useCreateSchedule()

  const aliveAgents = useMemo(() => agents.filter((a) => a.alive), [agents])

  const [agentId, setAgentId] = useState('')
  const [commandId, setCommandId] = useState('')
  const [label, setLabel] = useState('')
  const [frequency, setFrequency] = useState('3600')
  const [windowSec, setWindowSec] = useState('43200')
  const [paramValues, setParamValues] = useState<Record<string, string>>({})

  const selectedAgent = useMemo(
    () => agents.find((a) => a.agent_id === agentId) ?? null,
    [agents, agentId],
  )
  const commands = useAgentCommands(selectedAgent)
  const catalog = commands.data?.commands ?? []
  const selectedCommand = catalog.find((c) => c.command_id === commandId) ?? null

  const agentOptions = aliveAgents.map((a) => ({
    value: a.agent_id,
    label: a.agent_id,
  }))
  const commandOptions = catalog.map((c) => ({
    value: c.command_id,
    label: c.command_id,
  }))

  const reset = () => {
    setAgentId('')
    setCommandId('')
    setLabel('')
    setFrequency('3600')
    setWindowSec('43200')
    setParamValues({})
    create.reset()
  }

  const submit = () => {
    if (!agentId || !commandId) return
    const start = Date.now()
    create.mutate(
      {
        agent_id: agentId,
        command_id: commandId,
        label: label.trim() || undefined,
        params: Object.fromEntries(
          Object.entries(paramValues).filter(([, v]) => v.trim().length > 0),
        ),
        window_start_unix_ms: start,
        window_end_unix_ms: start + Number(windowSec) * 1000,
        interval_sec: Number(frequency),
      },
      {
        onSuccess: (data) => {
          if (data.success === false) {
            toast({
              title: 'Could not schedule command',
              description: data.error || 'Unknown error',
              variant: 'error',
            })
            return
          }
          toast({
            title: 'Command scheduled',
            description: `${commandId} on ${agentId} starting now.`,
            variant: 'success',
          })
          reset()
          onOpenChange(false)
        },
        onError: (err) => {
          toast({
            title: 'Could not schedule command',
            description: err instanceof Error ? err.message : 'Unknown error',
            variant: 'error',
          })
        },
      },
    )
  }

  const onParamChange = (key: string, value: string) => {
    setParamValues((prev) => ({ ...prev, [key]: value }))
  }

  return (
    <Dialog
      open={open}
      onOpenChange={(o) => {
        if (!o) {
          reset()
          create.reset()
        }
        onOpenChange(o)
      }}
      title="Schedule a command"
      description="Run a pre-set, whitelisted command on one agent repeatedly for a limited time — for example a speedtest every hour over the next 12 hours."
      maxWidthClass="max-w-xl"
    >
      <div className="space-y-4">
        <Select
          ariaLabel="Agent"
          value={agentId}
          onValueChange={(v) => {
            setAgentId(v)
            setCommandId('')
            setParamValues({})
          }}
          options={agentOptions}
          placeholder={
            aliveAgents.length === 0 ? 'No online agents' : 'Select an agent…'
          }
          disabled={aliveAgents.length === 0}
        />

        <Select
          ariaLabel="Command"
          value={commandId}
          onValueChange={(v) => {
            setCommandId(v)
            setParamValues({})
          }}
          options={commandOptions}
          placeholder={
            commands.isLoading
              ? 'Loading command catalog…'
              : !agentId
                ? 'Select an agent first…'
                : !selectedAgent?.alive
                  ? 'Agent offline — catalog unavailable'
                  : 'Select a command…'
          }
          disabled={!agentId || commands.isLoading || !selectedAgent?.alive}
        />

        {selectedCommand && (
          <div className="rounded-lg border border-border bg-bg p-3 text-xs text-fg-muted">
            {selectedCommand.description}
          </div>
        )}

        {selectedCommand && selectedCommand.param_names.length > 0 && (
          <div className="rounded-lg border border-border bg-bg p-3">
            <p className="mb-2 text-xs font-medium text-fg-muted">
              Parameters (optional — defaults apply when empty)
            </p>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              {selectedCommand.param_names.map((name) => (
                <Input
                  key={name}
                  label={name}
                  id={`param-${name}`}
                  value={paramValues[name] ?? ''}
                  onChange={(e) => onParamChange(name, e.target.value)}
                  placeholder={DEFAULT_PARAM_HINTS[name] ?? name}
                  hint={DEFAULT_PARAM_HINTS[name]}
                />
              ))}
            </div>
          </div>
        )}

        <Input
          label="Label (optional)"
          id="schedule-label"
          value={label}
          onChange={(e) => setLabel(e.target.value)}
          placeholder="e.g. evening bandwidth window"
        />

        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <Select
            ariaLabel="Frequency"
            value={frequency}
            onValueChange={setFrequency}
            options={FREQUENCY_OPTIONS}
          />
          <Select
            ariaLabel="Window"
            value={windowSec}
            onValueChange={setWindowSec}
            options={WINDOW_OPTIONS}
          />
        </div>

        <p className="flex items-center gap-1.5 text-xs text-fg-subtle">
          <CalendarClock className="size-3.5 shrink-0" aria-hidden="true" />
          Runs start right away and repeat inside the window. Offline agents
          skip their slot — the collector never bursts catch-up runs.
        </p>

        {create.isError && (
          <p
            className="rounded-lg border border-critical/40 bg-critical/10 px-3 py-2 text-xs text-critical"
            role="alert"
          >
            {create.error instanceof Error
              ? create.error.message
              : 'Failed to create the schedule'}
          </p>
        )}

        <div className="flex justify-end gap-2 pt-1">
          <Button variant="ghost" onClick={() => onOpenChange(false)}>
            Cancel
          </Button>
          <Button
            loading={create.isPending}
            disabled={!agentId || !commandId || create.isPending}
            onClick={submit}
          >
            {create.isPending ? (
              <>
                <Loader2 className="size-4 animate-spin" aria-hidden="true" />
                Scheduling…
              </>
            ) : (
              'Schedule command'
            )}
          </Button>
        </div>
      </div>
    </Dialog>
  )
}
