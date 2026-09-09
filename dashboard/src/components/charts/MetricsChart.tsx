import { useEffect, useMemo, useRef, useState } from 'react'
import {
  Area,
  AreaChart,
  Bar,
  Brush,
  CartesianGrid,
  ComposedChart,
  Line,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { LineChart as LineChartIcon } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useMetrics } from '../../hooks/useMetrics'
import { useChartTheme } from '../../hooks/useChartTheme'
import { CHECK_TYPE_LABELS, CHECK_TYPE_UNITS, CHART_LINE_COLORS } from '../../lib/constants'
import {
  buildChartData,
  buildChartLines,
  buildOutcomeRows,
  OUTCOME_FAIL_KEY,
  OUTCOME_OK_KEY,
  pickBucketMs,
} from '../../lib/derive'
import { cn } from '../../lib/cn'
import { ChartContainer } from './ChartContainer'
import { RechartsTooltip } from './RechartsTooltip'

/** Cap on simultaneously rendered series; the filter bar narrows the view beyond this. */
const MAX_VISIBLE_SERIES = 20

interface BrushRange {
  startIndex: number
  endIndex: number
}

export function MetricsChart() {
  const { selectedAgent, selectedCheck, windowSeconds } = useDashboard()
  const { data, isLoading, isError } = useMetrics({
    agentId: selectedAgent,
    checkType: selectedCheck,
    windowSeconds,
  })
  const chartTheme = useChartTheme()
  const [hidden, setHidden] = useState<Set<string>>(new Set())

  const filterKey = `${selectedAgent}\u0000${selectedCheck}\u0000${windowSeconds}`

  /**
   * Samples are aligned onto a shared grid of time buckets.
   */
  const { chartData, chartLines, totalSeries, hasFailures, maxFail } = useMemo(() => {
    const metrics = data ?? []
    const includeCheckType = selectedCheck === 'all'
    const bucketMs = pickBucketMs(windowSeconds, metrics)
    const toMs = Date.now()
    const rangeMs = { from: toMs - windowSeconds * 1000, to: toMs }
    const lines = buildChartLines(metrics, { includeCheckType })
    const seriesRows = buildChartData(metrics, { bucketMs, includeCheckType, rangeMs })
    const outcomeRows = buildOutcomeRows(metrics, { bucketMs, rangeMs })
    const outcomeByBucket = new Map(outcomeRows.map((row) => [row.time_ms, row]))

    const chartRows = seriesRows.map((row) => {
      const outcome = outcomeByBucket.get(row.time_ms)
      return outcome
        ? {
            ...row,
            [OUTCOME_OK_KEY]: Number(outcome[OUTCOME_OK_KEY]),
            [OUTCOME_FAIL_KEY]: Number(outcome[OUTCOME_FAIL_KEY]),
          }
        : row
    })

    let maxFail = 0
    for (const row of outcomeRows) {
      const fail = Number(row[OUTCOME_FAIL_KEY])
      if (fail > maxFail) maxFail = fail
    }

    return {
      chartData: chartRows,
      chartLines: lines.slice(0, MAX_VISIBLE_SERIES),
      totalSeries: lines.length,
      hasFailures: maxFail > 0,
      maxFail,
    }
  }, [data, selectedCheck, windowSeconds])

  /**
   * Controlled brush with a sensible default.
   */
  const [brush, setBrush] = useState<BrushRange | null>(null)
  const brushFilterKeyRef = useRef('')
  const brushLenRef = useRef(0)

  useEffect(() => {
    const len = chartData.length
    if (len === 0) {
      setBrush(null)
      brushLenRef.current = 0
      return
    }
    if (brushFilterKeyRef.current !== filterKey) {
      brushFilterKeyRef.current = filterKey
      brushLenRef.current = len
      const start = windowSeconds >= 3600 && len > 120 ? Math.floor(len * 0.7) : 0
      setBrush({ startIndex: start, endIndex: len - 1 })
      return
    }
    const prevLen = brushLenRef.current
    const delta = len - prevLen
    brushLenRef.current = len
    if (delta > 0) {
      setBrush((b) => {
        if (!b || b.endIndex < Math.max(0, prevLen - 1)) return b
        const startIndex = Math.min(b.startIndex + delta, len - 1)
        return { startIndex, endIndex: len - 1 }
      })
    }
  }, [chartData.length, filterKey, windowSeconds])

  const toggleLine = (line: string) => {
    setHidden((prev) => {
      const next = new Set(prev)
      if (next.has(line)) next.delete(line)
      else next.add(line)
      return next
    })
  }

  const visibleLines = chartLines.filter((l) => !hidden.has(l))
  const omittedSeries = Math.max(0, totalSeries - chartLines.length)
  const unit = selectedCheck !== 'all' ? CHECK_TYPE_UNITS[selectedCheck] : undefined
  const tooltipUnit = unit ? ` ${unit}` : undefined

  const showDate = windowSeconds >= 21600

  return (
    <ChartContainer
      title="Time-Series Metrics"
      subtitle={
        selectedCheck === 'all'
          ? 'Successful probes — one line per agent · check · target'
          : `${CHECK_TYPE_LABELS[selectedCheck]} — one line per agent · target`
      }
      isLoading={isLoading}
      isEmpty={chartLines.length === 0}
      emptyMessage={
        isError
          ? 'Metric storage is unreachable. Try again in a moment.'
          : 'No metric data available. Make sure an agent is running with probes configured.'
      }
      emptyIcon={<LineChartIcon className="size-8" aria-hidden="true" />}
      className={cn(isError && 'border-critical/40')}
    >
      {omittedSeries > 0 && (
        <p
          className="mb-3 rounded-lg border border-warning/30 bg-warning/10 px-3 py-1.5 text-xs text-warning"
          role="note"
        >
          Showing {chartLines.length} of {totalSeries} series. Narrow the agent or check filters to focus the chart.
        </p>
      )}

      {chartLines.length > 0 && (
        <div className="mb-3 flex flex-wrap gap-1.5" role="group" aria-label="Toggle chart series">
          {chartLines.map((line, i) => {
            const isHidden = hidden.has(line)
            const color = CHART_LINE_COLORS[i % CHART_LINE_COLORS.length]
            return (
              <button
                key={line}
                onClick={() => toggleLine(line)}
                aria-pressed={!isHidden}
                title={isHidden ? `Show ${line}` : `Hide ${line}`}
                className={cn(
                  'inline-flex items-center gap-1.5 rounded-full border px-2 py-0.5 text-[11px] font-medium transition-colors',
                  'focus:outline-none focus-visible:ring-2 focus-visible:ring-accent/40',
                  isHidden
                    ? 'border-border text-fg-subtle opacity-50'
                    : 'border-border text-fg hover:bg-surface-muted',
                )}
              >
                <span
                  className="size-2 rounded-full"
                  style={{ backgroundColor: isHidden ? '#9d9d9d' : color }}
                  aria-hidden="true"
                />
                <span className="max-w-40 truncate">{line}</span>
              </button>
            )
          })}
        </div>
      )}

      <ResponsiveContainer width="100%" height={320}>
        <ComposedChart data={chartData} margin={{ top: 10, right: 16, left: 0, bottom: 0 }}>
          <CartesianGrid strokeDasharray="3 3" stroke={chartTheme.grid} vertical={false} />
          <XAxis
            dataKey="time"
            tick={{ fill: chartTheme.axisText, fontSize: 11 }}
            tickLine={false}
            axisLine={{ stroke: chartTheme.grid }}
            minTickGap={40}
          />
          <YAxis
            tick={{ fill: chartTheme.axisText, fontSize: 11 }}
            tickLine={false}
            axisLine={false}
            width={48}
          />
          {/* Failure band, red bars behind the lines, scaled to their own axis. */}
          {hasFailures && (
            <>
              <YAxis
                yAxisId="outcome"
                hide
                width={0}
                domain={[0, Math.max(1, maxFail)]}
              />
              <Bar
                yAxisId="outcome"
                dataKey={OUTCOME_FAIL_KEY}
                name="Failed probes"
                fill="#f85149"
                fillOpacity={0.22}
                isAnimationActive={false}
              />
            </>
          )}
          <Tooltip
            content={
              <RechartsTooltip
                unit={tooltipUnit}
                showDate={showDate}
              />
            }
          />
          <Brush
            dataKey="time"
            height={36}
            stroke={chartTheme.brushStroke}
            fill={chartTheme.brushFill}
            travellerWidth={14}
            strokeWidth={1}
            startIndex={brush?.startIndex}
            endIndex={brush?.endIndex}
            onChange={(next) => {
              if (next) setBrush({ startIndex: next.startIndex, endIndex: next.endIndex })
            }}
          />
          {visibleLines.map((line, i) => (
            <Line
              key={line}
              type="monotone"
              dataKey={line}
              stroke={CHART_LINE_COLORS[i % CHART_LINE_COLORS.length]}
              strokeWidth={2}
              dot={false}
              isAnimationActive={false}
            />
          ))}
        </ComposedChart>
      </ResponsiveContainer>
      {hasFailures && (
        <p className="mt-1.5 text-right text-[11px] text-fg-subtle">
          <span className="mr-1 inline-block size-2 rounded-sm bg-critical/60 align-[-1px]" aria-hidden="true" />
          red bars mark buckets with failed probes
        </p>
      )}
    </ChartContainer>
  )
}

/** Compact area sparkline for agent cards. */
export function SparklineChart({
  data,
  color = '#3fb950',
  height = 36,
}: {
  data: { time_ms: number; value: number }[]
  color?: string
  height?: number
}) {
  if (data.length < 2) return null
  return (
    <ResponsiveContainer width="100%" height={height}>
      <AreaChart data={data} margin={{ top: 2, right: 0, left: 0, bottom: 0 }}>
        <YAxis hide domain={['auto', 'auto']} />
        <XAxis hide dataKey="time_ms" />
        <Area
          type="monotone"
          dataKey="value"
          stroke={color}
          strokeWidth={1.5}
          fill={color}
          fillOpacity={0.15}
          isAnimationActive={false}
        />
      </AreaChart>
    </ResponsiveContainer>
  )
}

