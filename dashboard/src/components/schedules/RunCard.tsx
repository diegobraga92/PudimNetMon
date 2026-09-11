import { useState } from 'react'
import {
  AlertTriangle,
  CheckCircle2,
  ChevronDown,
  ClipboardCopy,
  Loader2,
} from 'lucide-react'
import type { CommandRun } from '../../types'
import { cn } from '../../lib/cn'
import { copyText } from '../../lib/clipboard'
import { formatDateTime } from '../../lib/formatters'
import { Badge } from '../ui/Badge'
import { Button } from '../ui/Button'
import { Card } from '../ui/Card'
import { useToast } from '../ui/toast'

export function RunStatusBadge({ run }: { run: CommandRun }) {
  if (run.running) return <Badge variant="info">Running</Badge>
  if (run.success) return <Badge variant="success">Success</Badge>
  return <Badge variant="critical">Failed</Badge>
}

/**
 * One command execution. Collapsed: status + summary. Expanded: issues, the
 * full structured fields grid and the raw tool output (detail).
 */
export function RunCard({ run }: { run: CommandRun }) {
  const [open, setOpen] = useState(false)
  const { toast } = useToast()
  const fields = Object.entries(run.fields ?? {})
  const issues = run.issues ?? []

  const copyDetail = async () => {
    const ok = await copyText(run.detail || run.summary || run.error || '')
    if (ok) {
      toast({ title: 'Run output copied', variant: 'success' })
    } else {
      toast({ title: 'Copy failed — select the text manually', variant: 'error' })
    }
  }

  return (
    <Card className={cn('overflow-hidden p-0', open && 'ring-1 ring-accent/20')}>
      <button
        type="button"
        onClick={() => setOpen((v) => !v)}
        aria-expanded={open}
        className="flex w-full items-center gap-2 px-3 py-2.5 text-left transition-colors hover:bg-surface-muted focus:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-accent/40"
      >
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
          <span className="hidden truncate font-mono text-[11px] text-fg-subtle lg:inline" title={run.schedule_id}>
            {run.schedule_id}
          </span>
        )}
        <span className="ml-auto shrink-0 text-[11px] text-fg-subtle">{formatDateTime(run.started_unix_ms)}</span>
        <RunStatusBadge run={run} />
        <ChevronDown
          className={cn('size-3.5 shrink-0 text-fg-subtle transition-transform', open && 'rotate-180')}
          aria-hidden="true"
        />
      </button>

      {open && (
        <div className="space-y-2 border-t border-border bg-bg/60 px-3 py-3">
          {run.summary && <p className="text-xs text-fg-muted">{run.summary}</p>}
          {run.error && (
            <p className="rounded-md border border-critical/40 bg-critical/10 px-2 py-1.5 text-xs text-critical">
              {run.error}
            </p>
          )}

          {issues.length > 0 && (
            <ul className="space-y-1">
              {issues.map((issue, i) => (
                <li
                  key={i}
                  className="rounded-md border border-critical/40 bg-critical/10 px-2 py-1 text-xs text-critical"
                >
                  {issue}
                </li>
              ))}
            </ul>
          )}

          {fields.length > 0 && (
            <dl className="grid grid-cols-1 gap-x-6 gap-y-1 rounded-lg border border-border bg-surface p-3 text-xs sm:grid-cols-2">
              {fields.map(([key, value]) => (
                <div key={key} className="flex items-center justify-between gap-2">
                  <dt className="text-fg-muted">{key}</dt>
                  <dd className="truncate font-mono text-fg" title={value}>
                    {value}
                  </dd>
                </div>
              ))}
            </dl>
          )}

          {run.detail && (
            <div>
              <div className="mb-1 flex items-center justify-between gap-2">
                <span className="text-[11px] font-medium uppercase tracking-wide text-fg-subtle">
                  Raw output
                </span>
                <Button variant="ghost" size="sm" onClick={copyDetail}>
                  <ClipboardCopy className="size-3.5" aria-hidden="true" />
                  Copy
                </Button>
              </div>
              <pre className="max-h-56 overflow-auto whitespace-pre-wrap rounded-lg border border-border bg-surface p-3 font-mono text-[11px] leading-relaxed text-fg">
                {run.detail}
              </pre>
            </div>
          )}
        </div>
      )}
    </Card>
  )
}
