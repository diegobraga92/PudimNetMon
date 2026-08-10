import { useMemo, useState } from 'react'
import {
  Area,
  AreaChart,
  Brush,
  CartesianGrid,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { LineChart as LineChartIcon } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useMetrics } from '../../hooks/useMetrics'
import { useChartTheme } from '../../hooks/useChartTheme'
import { CHART_LINE_COLORS } from '../../lib/constants'
import { CHECK_TYPE_LABELS } from '../../lib/constants'
import { buildChartData, buildChartLines } from '../../lib/derive'
import { cn } from '../../lib/cn'
import { ChartContainer } from './ChartContainer'
import { RechartsTooltip } from './RechartsTooltip'

export function MetricsChart() {
  const { selectedAgent, selectedCheck, windowSeconds } = useDashboard()
  const { data, isLoading, isError } = useMetrics({
    agentId: selectedAgent,
    checkType: selectedCheck,
    windowSeconds,
  })
  const chartTheme = useChartTheme()
  const [hidden, setHidden] = useState<Set<string>>(new Set())

  const chartData = useMemo(() => buildChartData(data ?? []), [data])
  const chartLines = useMemo(() => buildChartLines(data ?? []), [data])

  const toggleLine = (line: string) => {
    setHidden((prev) => {
      const next = new Set(prev)
      if (next.has(line)) next.delete(line)
      else next.add(line)
      return next
    })
  }

  const visibleLines = chartLines.filter((l) => !hidden.has(l))

  return (
    <ChartContainer
      title="Time-Series Metrics"
      subtitle={
        selectedCheck === 'all'
          ? 'Successful probes, grouped by agent · target'
          : CHECK_TYPE_LABELS[selectedCheck]
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
        <LineChart data={chartData} margin={{ top: 10, right: 16, left: 0, bottom: 0 }}>
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
          <Tooltip content={<RechartsTooltip />} />
          <Brush
            dataKey="time"
            height={36}
            stroke={chartTheme.brushStroke}
            fill={chartTheme.brushFill}
            travellerWidth={14}
            strokeWidth={1}
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
        </LineChart>
      </ResponsiveContainer>
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

