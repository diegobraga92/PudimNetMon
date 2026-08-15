import { Check, Container, Copy } from 'lucide-react'
import { useState } from 'react'
import { cn } from '../../lib/cn'
import { useToast } from '../ui/toast'
import { collectorHost } from './PlatformCard'

const DOCKER_COMMAND = `docker run -d --name pudim-agent \\
  --restart unless-stopped \\
  pudimnetmon-agent:latest \\
  --collector-endpoint=${collectorHost()}:50051 \\
  --node-id=docker-1 --interval=5000`

export function DockerCard() {
  const { toast } = useToast()
  const [copied, setCopied] = useState(false)

  const handleCopy = async () => {
    try {
      await navigator.clipboard.writeText(DOCKER_COMMAND)
      setCopied(true)
      toast({ title: 'Docker command copied', variant: 'success' })
      window.setTimeout(() => setCopied(false), 2000)
    } catch {
      toast({ title: 'Copy failed — select the command manually', variant: 'error' })
    }
  }

  return (
    <div className="flex flex-col gap-3 rounded-xl border border-border bg-surface p-5 shadow-sm">
      <div className="flex items-start justify-between gap-3">
        <div>
          <h3 className="text-sm font-semibold text-fg">Docker</h3>
          <p className="mt-0.5 text-xs text-fg-muted">
            Any container host · build the image with <span className="font-mono">infra/docker/Dockerfile.agent</span>
          </p>
        </div>
        <span className="inline-flex h-7 items-center gap-1.5 rounded-md bg-surface-muted px-2.5 text-xs font-medium text-fg-muted">
          <Container className="size-3.5" aria-hidden="true" />
          Container
        </span>
      </div>

      <pre className="max-h-40 overflow-auto whitespace-pre-wrap rounded-lg border border-border bg-surface-muted p-3 font-mono text-xs text-fg">
        {DOCKER_COMMAND}
      </pre>

      <div className="flex items-center justify-between gap-3">
        <button
          onClick={handleCopy}
          className={cn(
            'inline-flex h-7 items-center gap-1.5 rounded-md px-2.5 text-xs font-medium transition-colors',
            'focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
            copied ? 'text-success' : 'text-fg-muted hover:bg-surface-muted hover:text-fg',
          )}
        >
          {copied ? <Check className="size-3.5" aria-hidden="true" /> : <Copy className="size-3.5" aria-hidden="true" />}
          {copied ? 'Copied' : 'Copy command'}
        </button>
      </div>
    </div>
  )
}
