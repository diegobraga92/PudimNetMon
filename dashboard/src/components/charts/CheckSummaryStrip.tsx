import { useMemo } from 'react'
import type { CheckTypeFilter, MetricPoint } from '../../types'
import { useDashboard } from '../../context/DashboardContext'
import { useMetrics } from '../../hooks/useMetrics'
import { CHECK_TYPE_COLORS, CHECK_TYPE_LABELS, CHECK_TYPE_UNITS } from '../../lib/constants'
import { pickBucketMs } from '../../lib/derive'
import { cn } from '../../lib/cn'
import { Card } from '../ui/Card'
import { SparklineChart } from './MetricsChart'

/** "DNS Resolution (ms)" -> "DNS Resolution". */
function shortLabel(label: string): string {
  return label.replace(/\s*\(ms\)$/, '')
}

function formatValue(value: number | undefined): string {
  if (value == null) return '—'
  if (value >= 100 || Number.isInteger(value)) return value.toLocaleString(undefined, { maximumFractionDigits: 0 })
  return value.toLocaleString(undefined, { maximumFractionDigits: 1 })
}

function availabilityTone(percent: number): string {
  if (percent >= 99.5) return 'text-success'
  if (percent >= 95) return 'text-warning'
  return 'text-critical'
}

interface Tile {
  total: number
  ok: number
  avg: number | undefined
  spark: { time_ms: number; value: number }[]
}

/** Aggregate one check type into avg latency + availability + sparkline over the window. */
function buildTile(all: MetricPoint[], windowSeconds: number): Tile {
  const ok = all.filter((m) => m.success)
  const avg = ok.length ? ok.reduce((sum, m) => sum + m.value, 0) / ok.length : undefined
  const bucketMs = pickBucketMs(windowSeconds, ok)
  const sums = new Map<number, { sum: number; count: number }>()
  for (const m of ok) {
    const bucket = Math.floor(m.time_ms / bucketMs) * bucketMs
    const acc = sums.get(bucket) ?? { sum: 0, count: 0 }
    acc.sum += m.value
    acc.count += 1
    sums.set(bucket, acc)
  }
  const spark = Array.from(sums.entries())
    .sort((a, b) => a[0] - b[0])
    .map(([bucket, acc]) => ({ time_ms: bucket, value: Math.round((acc.sum / acc.count) * 100) / 100 }))
  return { total: all.length, ok: ok.length, avg, spark }
}

interface TileData extends Tile {
  check: string
}

/** Compile per-check summaries from the shared "all checks" query for the current agent. */
function buildSummary(metrics: MetricPoint[], windowSeconds: number): TileData[] {
  const byCheck = new Map<string, MetricPoint[]>()
  for (const m of metrics) {
    const list = byCheck.get(m.check_type) ?? []
    list.push(m)
    byCheck.set(m.check_type, list)
  }
  return Object.keys(CHECK_TYPE_LABELS)
    .filter((check) => byCheck.has(check))
    .map((check) => ({ check, ...buildTile(byCheck.get(check) ?? [], windowSeconds) }))
}

export function CheckSummaryStrip() {
  const { selectedAgent, selectedCheck, windowSeconds, setSelectedCheck } = useDashboard()
  const { data, isLoading } = useMetrics({
    agentId: selectedAgent,
    checkType: 'all',
    windowSeconds,
  })

  const tiles = useMemo(() => buildSummary(data ?? [], windowSeconds), [data, windowSeconds])

  const focusChart = (check: CheckTypeFilter) => {
    setSelectedCheck(check)
    document.getElementById('time-series-chart')?.scrollIntoView?.({ behavior: 'smooth', block: 'start' })
  }

  if (isLoading && tiles.length === 0) {
    return (
      <section
        aria-label="Probe summary by check type"
        className="grid gap-2 sm:grid-cols-2 md:grid-cols-3 xl:grid-cols-4"
      >
        {Array.from({ length: 4 }).map((_, i) => (
          <div key={i} className="h-[92px] animate-pulse rounded-xl border border-border bg-surface-muted/60" />
        ))}
      </section>
    )
  }

  if (tiles.length === 0) return null

  return (
    <section
      aria-label="Probe summary by check type"
      className="grid gap-2 sm:grid-cols-2 md:grid-cols-3 xl:grid-cols-4"
    >
      {tiles.map((tile) => {
        const active = selectedCheck === tile.check
        const color = CHECK_TYPE_COLORS[tile.check] ?? '#8b949e'
        const percent = tile.total > 0 ? (tile.ok / tile.total) * 100 : 0
        const unit = CHECK_TYPE_UNITS[tile.check] ?? ''
        return (
          <Card
            key={tile.check}
            role="button"
            tabIndex={0}
            aria-pressed={active}
            onClick={() => focusChart(tile.check as CheckTypeFilter)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault()
                focusChart(tile.check as CheckTypeFilter)
              }
            }}
            className={cn(
              'group cursor-pointer p-3 transition-colors hover:border-border-strong',
              active && 'border-accent/50 ring-1 ring-accent/30',
            )}
          >
            <div className="flex items-center justify-between gap-2">
              <span className="flex min-w-0 items-center gap-1.5">
                <span
                  className="size-2 shrink-0 rounded-full"
                  style={{ backgroundColor: color }}
                  aria-hidden="true"
                />
                <span className="truncate text-xs font-medium text-fg" title={CHECK_TYPE_LABELS[tile.check]}>
                  {shortLabel(CHECK_TYPE_LABELS[tile.check])}
                </span>
              </span>
              {tile.total > 0 ? (
                <span className={cn('shrink-0 text-[11px] font-semibold', availabilityTone(percent))}>
                  {Math.round(percent)}%
                </span>
              ) : (
                <span className="shrink-0 text-[11px] text-fg-subtle">—</span>
              )}
            </div>

            {tile.spark.length >= 2 ? (
              <div className="mt-2">
                <SparklineChart data={tile.spark} color={color} height={30} />
              </div>
            ) : (
              <div
                className="mt-2 flex items-center justify-center rounded-md bg-surface-muted/70 text-[11px] text-fg-subtle"
                style={{ height: 30 }}
              >
                {tile.ok > 0 ? 'sampling…' : 'no data yet'}
              </div>
            )}

            <div className="mt-1.5 flex items-center justify-between text-[11px] text-fg-subtle">
              <span>
                avg {formatValue(tile.avg)}
                {unit && tile.avg != null ? ` ${unit}` : ''}
              </span>
              <span>{tile.total.toLocaleString()} probes</span>
            </div>
          </Card>
        )
      })}
    </section>
  )
}
