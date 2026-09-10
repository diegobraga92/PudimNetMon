import { useMemo } from 'react'
import { Clock } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useMetrics } from '../../hooks/useMetrics'
import { buildNtpAgents, buildNtpSeries, pickBucketMs } from '../../lib/derive'
import { cn } from '../../lib/cn'
import { Card } from '../ui/Card'
import { SparklineChart } from './MetricsChart'

/** Clock skew is only interesting when it drifts, so this stays a slim strip. */
const WINDOW_SECONDS = 3600

/** |offset| < 50ms reads as healthy, < 250ms as worth a look, else critical. */
function toneClass(offset: number): string {
  const abs = Math.abs(offset)
  if (abs < 50) return 'text-success'
  if (abs < 250) return 'text-warning'
  return 'text-critical'
}

function strokeColor(offset: number): string {
  const abs = Math.abs(offset)
  if (abs < 50) return '#3fb950'
  if (abs < 250) return '#f9ca24'
  return '#ff6b6b'
}

function formatOffset(value: number): string {
  const abs = Math.abs(value)
  const decimals = abs >= 100 ? 0 : abs >= 10 ? 1 : 2
  return `${value < 0 ? '\u2212' : '+'}${abs.toFixed(decimals)}`
}

interface AgentOffset {
  id: string
  spark: { time_ms: number; value: number }[]
  latest: number | null
}

/** Compact one-line NTP clock-skew strip: latest offset per agent + sparkline. */
export function NtpOffsetStrip() {
  const { selectedAgent } = useDashboard()
  const { data, isLoading } = useMetrics({
    agentId: selectedAgent,
    checkType: 'ntp_offset',
    windowSeconds: WINDOW_SECONDS,
  })

  const agents = useMemo<AgentOffset[]>(() => {
    const metrics = data ?? []
    const bucketMs = pickBucketMs(WINDOW_SECONDS, metrics)
    const series = buildNtpSeries(metrics, {
      bucketMs,
      rangeMs: { from: Date.now() - WINDOW_SECONDS * 1000, to: Date.now() },
    })
    return buildNtpAgents(metrics).map((id) => {
      const spark = series
        .filter((row) => typeof row[id] === 'number')
        .map((row) => ({ time_ms: row.time_ms, value: Number(row[id]) }))
      return { id, spark, latest: spark.length ? spark[spark.length - 1].value : null }
    })
  }, [data])

  // Keep the overview lean: nothing to show means nothing is rendered.
  if (isLoading || agents.length === 0) return null

  return (
    <Card className="flex flex-wrap items-center gap-x-6 gap-y-2 px-4 py-2.5">
      <span className="flex items-center gap-1.5 text-xs font-medium text-fg">
        <Clock className="size-3.5 text-fg-subtle" aria-hidden="true" />
        NTP Offset
      </span>
      {agents.map((agent) => (
        <span key={agent.id} className="flex items-center gap-2">
          <span className="max-w-40 truncate font-mono text-[11px] text-fg-muted" title={agent.id}>
            {agent.id}
          </span>
          <span className="w-20 shrink-0">
            <SparklineChart
              data={agent.spark}
              color={agent.latest != null ? strokeColor(agent.latest) : '#8b949e'}
              height={18}
            />
          </span>
          <span
            className={cn(
              'font-mono text-[11px]',
              agent.latest != null ? toneClass(agent.latest) : 'text-fg-subtle',
            )}
          >
            {agent.latest != null ? `${formatOffset(agent.latest)} ms` : '—'}
          </span>
        </span>
      ))}
    </Card>
  )
}
