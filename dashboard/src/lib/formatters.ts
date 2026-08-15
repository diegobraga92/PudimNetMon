/** Human-friendly formatting helpers. */

export function formatTime(unixMs: number): string {
  return new Date(unixMs).toLocaleTimeString()
}

export function formatDateTime(unixMs: number): string {
  return new Date(unixMs).toLocaleString()
}

/** "just now", "42s ago", "12m ago", "3h ago", "4d ago". */
export function formatRelativeTime(unixMs: number, now = Date.now()): string {
  const diffSec = Math.max(0, Math.floor((now - unixMs) / 1000))
  if (diffSec < 5) return 'just now'
  if (diffSec < 60) return `${diffSec}s ago`
  const diffMin = Math.floor(diffSec / 60)
  if (diffMin < 60) return `${diffMin}m ago`
  const diffHr = Math.floor(diffMin / 60)
  if (diffHr < 24) return `${diffHr}h ago`
  return `${Math.floor(diffHr / 24)}d ago`
}

/** Format a number with locale separators and a bounded number of decimals. */
export function formatNumber(value: number, digits = 0): string {
  if (Number.isNaN(value)) return '—'
  if (!Number.isFinite(value)) return '∞'
  return value.toLocaleString(undefined, {
    maximumFractionDigits: digits,
  })
}

/** "900 B", "1.4 kB", "2.1 MB". */
export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '—'
  if (bytes < 1024) return `${formatNumber(bytes)} B`
  const units = ['kB', 'MB', 'GB', 'TB']
  let value = bytes
  let unit = 'B'
  for (const u of units) {
    if (value < 1024) break
    value /= 1024
    unit = u
  }
  return `${formatNumber(value, value >= 100 ? 0 : 1)} ${unit}`
}
