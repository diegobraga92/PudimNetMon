import { Check, Copy, Download } from 'lucide-react'
import { useState } from 'react'
import { cn } from '../../lib/cn'
import { formatBytes } from '../../lib/formatters'
import { useToast } from '../ui/toast'
import type { AgentPlatform } from '../../types'

/** Collector host reachable from the browser (used to build install commands). */
export function collectorHost(): string {
  if (typeof window === 'undefined') return 'localhost'
  return window.location.hostname || 'localhost'
}

/** Shell command that downloads the agent on the target host and runs it. */
export function installCommand(platform: AgentPlatform): string {
  const host = collectorHost()
  const url = `http://${host}:8080${platform.download_url}`
  const endpoint = `${host}:50051`
  if (platform.os === 'windows') {
    return [
      `curl.exe -fL -o ${platform.filename} "${url}"`,
      `.\\${platform.filename} --collector-endpoint=${endpoint} --node-id=my-host --interval=5000`,
    ].join('\n')
  }
  return [
    `curl -fL -o ${platform.filename} "${url}"`,
    `chmod +x ${platform.filename}`,
    // The staged binary is built on Ubuntu 24.04 (the collector image base);
    // install the matching runtime libs so it links on Debian/Ubuntu hosts.
    'sudo apt-get update',
    'sudo apt-get install -y libgrpc++1.51t64 libprotobuf32t64 libcurl4t64 \\',
    '  libpcap0.8t64 libsystemd0 libsqlite3-0',
    `./${platform.filename} --collector-endpoint=${endpoint} --node-id=$(hostname) --interval=5000`,
  ].join('\n')
}

async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    // fall through to the execCommand fallback
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

function osLabel(os: string): string {
  if (os === 'linux') return 'Linux'
  if (os === 'windows') return 'Windows'
  if (os === 'darwin') return 'macOS'
  if (os === 'freebsd') return 'FreeBSD'
  return os
}

export function PlatformCard({ platform }: { platform: AgentPlatform }) {
  const { toast } = useToast()
  const [copied, setCopied] = useState(false)
  const cmd = installCommand(platform)

  const handleCopy = async () => {
    const ok = await copyText(cmd)
    if (ok) {
      setCopied(true)
      toast({ title: 'Install command copied', variant: 'success' })
      window.setTimeout(() => setCopied(false), 2000)
    } else {
      toast({ title: 'Copy failed — select the command manually', variant: 'error' })
    }
  }

  return (
    <div className="flex flex-col gap-3 rounded-xl border border-border bg-surface p-5 shadow-sm">
      <div className="flex items-start justify-between gap-3">
        <div>
          <h3 className="text-sm font-semibold text-fg">{osLabel(platform.os)}</h3>
          <p className="mt-0.5 text-xs text-fg-muted">
            {platform.arch} · {platform.filename} · {formatBytes(platform.size_bytes)}
          </p>
        </div>
        <a
          href={platform.download_url}
          download
          className={cn(
            'inline-flex h-9 items-center gap-2 rounded-lg bg-accent px-4 text-sm font-medium text-white transition-colors',
            'hover:bg-accent/90 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
          )}
        >
          <Download className="size-4" aria-hidden="true" />
          Download
        </a>
      </div>

      <pre className="max-h-40 overflow-auto whitespace-pre-wrap rounded-lg border border-border bg-surface-muted p-3 font-mono text-xs text-fg">
        {cmd}
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
        <span className="font-mono text-[10px] text-fg-subtle" title={platform.sha256}>
          sha256 {platform.sha256.slice(0, 16)}…
        </span>
      </div>
      {platform.os === 'linux' && (
        <p className="text-[11px] text-fg-subtle">
          Built on Ubuntu 24.04 (the collector image base). On other distros, build from
          source instead — see <span className="font-mono">docs/deployment.md</span>.
        </p>
      )}
    </div>
  )
}
