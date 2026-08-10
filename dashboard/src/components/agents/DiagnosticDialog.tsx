import { useEffect } from 'react'
import { Beaker, ClipboardCopy, Loader2 } from 'lucide-react'
import type { AgentInfo } from '../../types'
import { useRunDiagnostic } from '../../hooks/useDiagnostics'
import { Button } from '../ui/Button'
import { Dialog } from '../ui/Dialog'
import { formatTime } from '../../lib/formatters'

interface DiagnosticDialogProps {
  agent: AgentInfo | null
  open: boolean
  onOpenChange: (open: boolean) => void
  onResult: (agentId: string, result: { success: boolean; timestamp_unix_ms: number; result: string; error?: string }) => void
}

/** Runs a diagnostic against an agent and shows the output with a copy button. */
export function DiagnosticDialog({ agent, open, onOpenChange, onResult }: DiagnosticDialogProps) {
  const run = useRunDiagnostic()

  useEffect(() => {
    if (open && agent) {
      run.mutate(agent, {
        onSuccess: (data) => onResult(agent.agent_id, data),
        onError: (err) =>
          onResult(agent.agent_id, {
            success: false,
            timestamp_unix_ms: Date.now(),
            result: '',
            error: err instanceof Error ? err.message : 'diagnostic failed',
          }),
      })
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, agent?.agent_id])

  const copy = async () => {
    if (!agent || !run.data) return
    try {
      await navigator.clipboard.writeText(run.data.result || run.data.error || '')
    } catch {
      // clipboard unavailable — ignore
    }
  }

  return (
    <Dialog
      open={open}
      onOpenChange={(o) => {
        onOpenChange(o)
        if (!o) run.reset()
      }}
      title={`Diagnostic · ${agent?.agent_id ?? ''}`}
      description="Runs a quick network trace + pcap capture against example.com on the agent."
      maxWidthClass="max-w-xl"
    >
      {!agent ? null : run.isPending ? (
        <div className="flex flex-col items-center gap-3 py-8 text-fg-muted">
          <Loader2 className="size-6 animate-spin" aria-hidden="true" />
          <p className="text-sm">Running diagnostic…</p>
        </div>
      ) : run.error != null || (run.data != null && !run.data.success) ? (
        <div>
          <p className="rounded-lg border border-critical/40 bg-critical/10 px-3 py-2 text-sm text-critical">
            {run.error instanceof Error ? run.error.message : run.data?.error ?? 'Diagnostic failed'}
          </p>
        </div>
      ) : run.data && (
        <div>
          <div className="mb-2 flex items-center justify-between gap-2">
            <span className="flex items-center gap-2 text-xs text-fg-muted">
              <Beaker className="size-3.5 text-success" aria-hidden="true" />
              Completed at {formatTime(run.data.timestamp_unix_ms)}
            </span>
            <Button variant="ghost" size="sm" onClick={copy}>
              <ClipboardCopy className="size-3.5" aria-hidden="true" />
              Copy
            </Button>
          </div>
          <pre className="max-h-80 overflow-auto rounded-lg border border-border bg-bg p-3 font-mono text-xs leading-relaxed text-fg">
            {run.data.result}
          </pre>
        </div>
      )}
    </Dialog>
  )
}
