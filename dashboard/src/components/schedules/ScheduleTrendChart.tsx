import { useMemo, useState } from 'react'
import {
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { LineChart as LineChartIcon } from 'lucide-react'
import type { CommandRun } from '../../types'
import { useChartTheme } from '../../hooks/useChartTheme'
import { formatTime } from '../../lib/formatters'
import { Select, type SelectOption } from '../ui/Select'

/** Preferred metric order per command, so the default chart series is useful. */
const PREFERRED_SERIES: Record<string, string[]> = {
  speedtest: ['download_mbps', 'upload_mbps', 'ping_ms', 'jitter_ms', 'packet_loss_pct'],
  ping_burst: ['packet_loss_pct', 'rtt_avg_ms', 'jitter_ms', 'rtt_min_ms', 'rtt_max_ms'],
  route_quality: ['worst_hop_loss_pct', 'hops'],
  hdd_check: ['disk_usage_percent'],
}

/** Non-numeric output fields that should never be plotted as a trend. */
const TEXT_FIELDS = new Set(['server', 'server_id', 'isp', 'target', 'tool', 'note', 'volume', 'hostname'])

function metricLabel(key: string): string {
  return key.replace(/_/g, ' ')
}

function metricUnit(key: string): string {
  if (key.endsWith('_ms')) return 'ms'
  if (key.endsWith('_mbps')) return 'Mbps'
  if (key.endsWith('_pct')) return '%'
  if (key.endsWith('_bytes')) return 'bytes'
  if (key.endsWith('_sec')) return 's'
  return ''
}

function isNumericValue(value: string | undefined): boolean {
  if (!value) return false
  const n = Number(value)
  return value.trim() !== '' && Number.isFinite(n)
}

function collectMetricCandidates(runs: CommandRun[], commandId: string): string[] {
  const present = new Set<string>()
  for (const run of runs) {
    if (run.running || !run.success) continue
    for (const [key, value] of Object.entries(run.fields ?? {})) {
      if (!isNumericValue(value) || TEXT_FIELDS.has(key)) continue
      present.add(key)
    }
  }
  const preferred = PREFERRED_SERIES[commandId] ?? []
  const ordered = preferred.filter((k) => present.has(k))
  for (const key of present) {
    if (!ordered.includes(key)) ordered.push(key)
  }
  return ordered
}

interface ScheduleTrendChartProps {
  commandId: string
  runs: CommandRun[]
}

/**
 * Time-series of one structured numeric field across a schedule's successful
 * runs. Failed runs stay visible in the history list below the chart.
 */
export function ScheduleTrendChart({ commandId, runs }: ScheduleTrendChartProps) {
  const chartTheme = useChartTheme()
  const metrics = useMemo(() => collectMetricCandidates(runs, commandId), [runs, commandId])
  const [metric, setMetric] = useState('')

  // Default to the command's first preferred metric once candidates are known.
  const active = metric && metrics.includes(metric) ? metric : metrics[0] ?? ''
  const unit = metricUnit(active)

  const points = useMemo(() => {
    if (!active) return []
    return runs
      .filter((r) => r.running !== true && r.success)
      .map((r) => {
        const raw = r.fields?.[active]
        if (!isNumericValue(raw)) return null
        return { time_ms: r.started_unix_ms || r.scheduled_unix_ms, value: Number(raw) }
      })
      .filter((p): p is { time_ms: number; value: number } => p !== null)
      .sort((a, b) => a.time_ms - b.time_ms)
  }, [runs, active])

  if (metrics.length === 0) {
    return (
      <div className="flex items-center gap-2 rounded-lg border border-border bg-bg px-3 py-4 text-xs text-fg-muted">
        <LineChartIcon className="size-4 shrink-0 text-fg-subtle" aria-hidden="true" />
        No numeric results to plot yet — run this schedule a couple of times and its trend will appear here.
      </div>
    )
  }

  const metricOptions: SelectOption[] = metrics.map((key) => {
    const u = metricUnit(key)
    return { value: key, label: u ? `${metricLabel(key)} (${u})` : metricLabel(key) }
  })

  return (
    <div className="space-y-2">
      {metricOptions.length > 1 && (
        <div className="flex max-w-64 items-center gap-2">
          <Select
            ariaLabel="Chart metric"
            value={active}
            onValueChange={setMetric}
            options={metricOptions}
          />
        </div>
      )}
      {points.length < 2 ? (
        <div className="rounded-lg border border-border bg-bg px-3 py-4 text-xs text-fg-muted">
          Need at least two successful runs of <span className="font-mono">{active}</span> to
          draw a trend.
        </div>
      ) : (
        <ResponsiveContainer width="100%" height={160}>
          <LineChart data={points} margin={{ top: 6, right: 8, left: 0, bottom: 0 }}>
            <CartesianGrid strokeDasharray="3 3" stroke={chartTheme.grid} vertical={false} />
            <XAxis
              dataKey="time_ms"
              scale="time"
              type="number"
              domain={['dataMin', 'dataMax']}
              tick={{ fill: chartTheme.axisText, fontSize: 11 }}
              tickLine={false}
              axisLine={{ stroke: chartTheme.grid }}
              tickFormatter={(t: number) => formatTime(t)}
              minTickGap={48}
            />
            <YAxis hide domain={['auto', 'auto']} />
            <Tooltip
              cursor={{ stroke: chartTheme.grid }}
              content={({ active: a, payload }) => {
                if (!a || !payload || payload.length === 0) return null
                const datum = payload[0]?.payload as
                  | { time_ms: number; value: number }
                  | undefined
                if (!datum) return null
                return (
                  <div className="rounded-lg border border-border bg-surface-raised px-3 py-2 text-xs shadow-xl">
                    <p className="font-semibold text-fg">{formatTime(datum.time_ms)}</p>
                    <p className="flex items-center gap-2 py-0.5 text-fg-muted">
                      <span
                        className="inline-block size-2 rounded-full"
                        style={{ backgroundColor: '#4ecdc4' }}
                        aria-hidden="true"
                      />
                      {metricLabel(active)}
                      <span className="ml-auto font-medium text-fg">
                        {datum.value.toLocaleString(undefined, { maximumFractionDigits: 2 })}
                        {unit ? ` ${unit}` : ''}
                      </span>
                    </p>
                  </div>
                )
              }}
            />
            <Line
              type="monotone"
              dataKey="value"
              stroke="#4ecdc4"
              strokeWidth={2}
              dot={{ r: 2.5, fill: '#4ecdc4', strokeWidth: 0 }}
              isAnimationActive={false}
            />
          </LineChart>
        </ResponsiveContainer>
      )}
      <p className="text-[11px] text-fg-subtle">
        {metricLabel(active)}
        {unit ? ` (${unit})` : ''} per run · failed or incomplete runs are excluded from the
        line.
      </p>
    </div>
  )
}
