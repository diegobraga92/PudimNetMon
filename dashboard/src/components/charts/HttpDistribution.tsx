import { Bar, BarChart, CartesianGrid, Legend, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts'
import { BarChart3 } from 'lucide-react'
import { useMemo } from 'react'
import { useMetrics } from '../../hooks/useMetrics'
import { useChartTheme } from '../../hooks/useChartTheme'
import { useDashboard } from '../../context/DashboardContext'
import { buildProtocolComparison } from '../../lib/derive'
import { ChartContainer } from './ChartContainer'
import { RechartsTooltip } from './RechartsTooltip'

const PROTOCOL_BARS = [
  { dataKey: 'http11', name: 'HTTP/1.1', fill: '#45b7d1' },
  { dataKey: 'http2', name: 'HTTP/2', fill: '#4ecdc4' },
  { dataKey: 'http3', name: 'HTTP/3', fill: '#a29bfe' },
]

export function HttpDistribution() {
  const { selectedAgent } = useDashboard()
  const { data, isLoading } = useMetrics({
    agentId: selectedAgent,
    checkType: 'http_request',
    windowSeconds: 3600,
  })
  const chartTheme = useChartTheme()
  const groups = useMemo(() => buildProtocolComparison(data ?? []), [data])

  return (
    <ChartContainer
      title="HTTP Protocol Comparison"
      subtitle="Latency in ms per protocol — latest probe"
      isLoading={isLoading}
      isEmpty={groups.length === 0}
      emptyMessage="No per-protocol data yet. Run an agent with --http-protocols=http1.1,http2,http3"
      emptyIcon={<BarChart3 className="size-8" aria-hidden="true" />}
    >
      <ResponsiveContainer width="100%" height={260}>
        <BarChart data={groups} margin={{ top: 10, right: 16, left: 0, bottom: 0 }} barGap={4}>
          <CartesianGrid strokeDasharray="3 3" stroke={chartTheme.grid} vertical={false} />
          <XAxis
            dataKey="url"
            tick={{ fill: chartTheme.axisText, fontSize: 11 }}
            tickLine={false}
            axisLine={{ stroke: chartTheme.grid }}
            interval={0}
            tickFormatter={(value: string) => (value.length > 22 ? `${value.slice(0, 21)}…` : value)}
          />
          <YAxis
            tick={{ fill: chartTheme.axisText, fontSize: 11 }}
            tickLine={false}
            axisLine={false}
            width={48}
          />
          <Tooltip content={<RechartsTooltip unit=" ms" />} cursor={{ fill: chartTheme.grid, opacity: 0.25 }} />
          <Legend
            wrapperStyle={{ fontSize: 12, color: chartTheme.axisText }}
            iconType="circle"
            iconSize={8}
          />
          {PROTOCOL_BARS.map((bar) => (
            <Bar key={bar.dataKey} dataKey={bar.dataKey} name={bar.name} fill={bar.fill} radius={[3, 3, 0, 0]} />
          ))}
        </BarChart>
      </ResponsiveContainer>
    </ChartContainer>
  )
}
