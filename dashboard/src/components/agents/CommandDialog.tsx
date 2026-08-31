import { useState } from 'react'
import {
  Activity,
  AlertTriangle,
  CheckCircle2,
  ClipboardCopy,
  Loader2,
  TerminalSquare,
} from 'lucide-react'
import type { AgentCommand, AgentInfo, CommandResult } from '../../types'
import { useAgentCommands, useRunAgentCommand } from '../../hooks/useAgentCommands'
import { Button } from '../ui/Button'
import { Dialog } from '../ui/Dialog'
import { formatTime } from '../../lib/formatters'

interface CommandDialogProps {
  agent: AgentInfo | null
  open: boolean
  onOpenChange: (open: boolean) => void
}

/**
 * Lists the agent's pre-set (whitelisted) commands and runs one on demand.
 * Commands come from the agent's fixed catalog — never arbitrary shell input.
 */
export function CommandDialog({ agent, open, onOpenChange }: CommandDialogProps) {
  const commands = useAgentCommands(agent)
  const run = useRunAgentCommand()
  const [selected, setSelected] = useState<AgentCommand | null>(null)

  return (
    <Dialog
      open={open}
      onOpenChange={(o) => {
        onOpenChange(o)
        if (!o) {
          run.reset()
          setSelected(null)
        }
      }}
      title={`Agent commands · ${agent?.agent_id ?? ''}`}
      description="Run a pre-set, whitelisted maintenance command on the agent. The agent only executes commands from its fixed catalog — no arbitrary shell/terminal input."
      maxWidthClass="max-w-2xl"
    >
      {!agent ? null : commands.isLoading ? (
        <div className="flex items-center gap-2 py-8 text-sm text-fg-muted">
          <Loader2 className="size-4 animate-spin" aria-hidden="true" />
          Loading command catalog…
        </div>
      ) : (commands.data?.commands.length ?? 0) > 0 ? (
        <div className="space-y-3">
          {commands.data!.commands.map((cmd) => {
            const isRunning = run.isPending && selected?.command_id === cmd.command_id
            return (
              <div key={cmd.command_id} className="rounded-lg border border-border bg-bg p-3">
                <div className="flex items-center justify-between gap-2">
                  <div className="min-w-0">
                    <p className="flex items-center gap-2 text-sm font-medium text-fg">
                      <TerminalSquare className="size-3.5 shrink-0 text-fg-muted" aria-hidden="true" />
                      {cmd.command_id}
                    </p>
                    <p className="mt-0.5 text-xs text-fg-muted">{cmd.description}</p>
                  </div>
                  <Button
                    size="sm"
                    loading={isRunning}
                    disabled={run.isPending}
                    onClick={() => {
                      setSelected(cmd)
                      run.mutate({ agent, command: cmd })
                    }}
                  >
                    <Activity className="size-3.5" aria-hidden="true" />
                    Run
                  </Button>
                </div>

                {selected?.command_id === cmd.command_id && run.isSuccess && run.data && (
                  <CommandResultView result={run.data} onReset={run.reset} />
                )}
                {selected?.command_id === cmd.command_id && run.isError && (
                  <p className="mt-3 rounded-lg border border-critical/40 bg-critical/10 px-3 py-2 text-xs text-critical">
                    {run.error instanceof Error ? run.error.message : 'Command failed'}
                  </p>
                )}
              </div>
            )
          })}
        </div>
      ) : (
        <p className="py-8 text-sm text-fg-muted">
          {commands.isError
            ? 'Could not load the command catalog.'
            : 'This agent exposes no pre-set commands.'}
        </p>
      )}
    </Dialog>
  )
}

function CommandResultView({
  result,
  onReset,
}: {
  result: CommandResult
  onReset: () => void
}) {
  const fields = result.fields ?? {}
  const issues = result.issues ?? []
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(result.detail || result.summary || '')
    } catch {
      // clipboard unavailable — ignore
    }
  }

  return (
    <div className="mt-3 space-y-2">
      <div className="flex items-center justify-between gap-2">
        <span className="flex items-center gap-2 text-xs text-fg-muted">
          {result.success ? (
            <CheckCircle2 className="size-3.5 text-success" aria-hidden="true" />
          ) : (
            <AlertTriangle className="size-3.5 text-critical" aria-hidden="true" />
          )}
          {result.success ? result.summary || 'ok' : result.error || 'failed'} ·{' '}
          {formatTime(result.timestamp_unix_ms)}
        </span>
        <Button variant="ghost" size="sm" onClick={copy}>
          <ClipboardCopy className="size-3.5" aria-hidden="true" />
          Copy
        </Button>
      </div>

      {issues.length > 0 && (
        <ul className="space-y-1">
          {issues.map((issue, i) => (
            <li
              key={i}
              className="rounded border border-critical/40 bg-critical/10 px-2 py-1 text-xs text-critical"
            >
              {issue}
            </li>
          ))}
        </ul>
      )}

      {Object.keys(fields).length > 0 && (
        <dl className="grid grid-cols-1 gap-x-6 gap-y-1 rounded-lg border border-border bg-bg p-3 text-xs sm:grid-cols-2">
          {Object.entries(fields).map(([key, value]) => (
            <div key={key} className="flex items-center justify-between gap-2">
              <dt className="text-fg-muted">{key}</dt>
              <dd className="truncate font-mono text-fg" title={value}>
                {value}
              </dd>
            </div>
          ))}
        </dl>
      )}

      {result.detail && (
        <pre className="max-h-48 overflow-auto rounded-lg border border-border bg-bg p-3 font-mono text-[11px] leading-relaxed text-fg">
          {result.detail}
        </pre>
      )}

      <div className="flex justify-end">
        <Button variant="ghost" size="sm" onClick={onReset}>
          Dismiss
        </Button>
      </div>
    </div>
  )
}

