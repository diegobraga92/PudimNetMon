import { useEffect, useState } from 'react'
import { formatRelativeTime } from '../../lib/formatters'
import { cn } from '../../lib/cn'
import { POLL_INTERVAL_MS } from '../../lib/constants'

interface LastUpdatedProps {
  /** Unix ms timestamp of the latest successful fetch. */
  timestamp?: number | null
  /** True while no data has ever arrived. */
  loading?: boolean
  /** Override the freshness threshold at which the label turns warning (ms). */
  staleAfterMs?: number
}

/** Live "Updated X ago" pill; turns amber when data is stale and red when old. */
export function LastUpdated({ timestamp, loading, staleAfterMs = POLL_INTERVAL_MS + 3000 }: LastUpdatedProps) {
  const [, setTick] = useState(0)

  useEffect(() => {
    const id = window.setInterval(() => setTick((t) => t + 1), 1000)
    return () => window.clearInterval(id)
  }, [])

  if (loading && timestamp == null) {
    return <span className="text-xs text-fg-subtle">Connecting…</span>
  }
  if (timestamp == null) {
    return <span className="text-xs text-critical">No data</span>
  }

  const ageMs = Date.now() - timestamp
  const stale = ageMs > staleAfterMs
  const veryStale = ageMs > staleAfterMs * 2

  return (
    <span
      className={cn(
        'inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-xs transition-colors',
        veryStale
          ? 'border-critical/40 bg-critical/10 text-critical'
          : stale
            ? 'border-warning/40 bg-warning/10 text-warning'
            : 'border-border bg-surface-muted text-fg-muted',
      )}
      title={`Last refreshed ${new Date(timestamp).toLocaleTimeString()}`}
    >
      <span
        className={cn('size-1.5 rounded-full', veryStale ? 'bg-critical' : stale ? 'bg-warning' : 'bg-success')}
        aria-hidden="true"
      />
      Updated {formatRelativeTime(timestamp)}
    </span>
  )
}
