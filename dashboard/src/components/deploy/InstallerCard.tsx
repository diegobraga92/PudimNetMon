import { Check, Copy, Download } from 'lucide-react'
import { useState } from 'react'
import { cn } from '../../lib/cn'
import { decoratedInstallerFilename, encodeInstallConfig } from '../../lib/installConfig'
import { formatBytes } from '../../lib/formatters'
import { useToast } from '../ui/toast'
import type { AgentInstaller } from '../../types'
import { collectorHost } from './PlatformCard'

function osLabel(os: string): string {
  if (os === 'linux') return 'Linux'
  if (os === 'windows') return 'Windows'
  if (os === 'darwin') return 'macOS'
  return os
}

function kindLabel(kind: string): string {
  if (kind === 'setup') return 'Setup wizard'
  if (kind === 'run') return 'Self-extracting installer'
  return kind
}

async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    // Fall back to the execCommand copy.
  }
  try {
    const ta = document.createElement('textarea')
    ta.value = text
    ta.setAttribute('readonly', '')
    ta.style.position = 'fixed'
    ta.style.opacity = '0'
    document.body.appendChild(ta)
    ta.select()
    const ok = document.execCommand('copy')
    document.body.removeChild(ta)
    return ok
  } catch {
    return false
  }
}

/**
 * One-click installer card. The collector endpoint is baked into a base64url
 * token that the collector embeds in the saved file name, so the downloaded
 * artifact installs with the right collector (and optional node id) by itself.
 */
export function InstallerCard({ installer }: { installer: AgentInstaller }) {
  const { toast } = useToast()
  const [copied, setCopied] = useState(false)
  const [nodeId, setNodeId] = useState('')

  const host = collectorHost()
  const endpoint = `${host}:50051`
  const config = encodeInstallConfig({ collector: endpoint, node: nodeId || undefined })
  const downloadUrl = `${installer.download_url}&config=${encodeURIComponent(config)}`
  const savedName = decoratedInstallerFilename(installer.filename, config)

  const handleCopy = async () => {
    const ok = await copyText(`${window.location.origin}${downloadUrl}`)
    if (ok) {
      setCopied(true)
      toast({ title: 'Download link copied', variant: 'success' })
      window.setTimeout(() => setCopied(false), 2000)
    } else {
      toast({ title: 'Copy failed — select the link manually', variant: 'error' })
    }
  }

  const runHint =
    installer.os === 'linux'
      ? `chmod +x "${savedName}" && sudo ./"${savedName}"`
      : `Run "${savedName}" on the target Windows host`

  return (
    <div className="flex flex-col gap-3 rounded-xl border border-border bg-surface p-5 shadow-sm">
      <div className="flex items-start justify-between gap-3">
        <div>
          <div className="flex items-center gap-2">
            <h3 className="text-sm font-semibold text-fg">{osLabel(installer.os)}</h3>
            <span className="rounded-full border border-border bg-surface-muted px-2 py-0.5 text-[10px] font-medium uppercase tracking-wide text-fg-subtle">
              {kindLabel(installer.kind)}
            </span>
          </div>
          <p className="mt-0.5 text-xs text-fg-muted">
            {installer.arch} · {installer.filename} · {formatBytes(installer.size_bytes)}
          </p>
        </div>
        <a
          href={downloadUrl}
          className={cn(
            'inline-flex h-9 shrink-0 items-center gap-2 rounded-lg bg-accent px-4 text-sm font-medium text-white transition-colors',
            'hover:bg-accent/90 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
          )}
          title={`Saves as ${savedName}`}
        >
          <Download className="size-4" aria-hidden="true" />
          Download
        </a>
      </div>

      <label className="flex flex-col gap-1">
        <span className="text-xs font-medium text-fg-muted">
          Node ID <span className="text-fg-subtle">(optional — auto when empty)</span>
        </span>
        <input
          type="text"
          value={nodeId}
          onChange={(e) => setNodeId(e.target.value)}
          placeholder="e.g. web-01"
          className="h-8 rounded-md border border-border bg-surface-muted px-2 font-mono text-xs text-fg outline-none focus:border-accent/60"
        />
      </label>

      <pre className="max-h-40 overflow-auto whitespace-pre-wrap rounded-lg border border-border bg-surface-muted p-3 font-mono text-xs text-fg">
        {runHint}
      </pre>

      <p className="text-[11px] text-fg-subtle" title={installer.sha256}>
        sha256 {installer.sha256.slice(0, 16)}… · saved as{' '}
        <span className="font-mono">{savedName}</span>
      </p>

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
          {copied ? 'Copied' : 'Copy link'}
        </button>
        <span className="text-[11px] text-fg-subtle">
          Collector baked in: <span className="font-mono">{endpoint}</span>
        </span>
      </div>
    </div>
  )
}
