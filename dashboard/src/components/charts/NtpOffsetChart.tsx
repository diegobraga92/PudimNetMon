import { Clock } from 'lucide-react'
import { CartesianGrid, Legend, Line, LineChart, ReferenceLine, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts'
import { useMemo } from 'react'
import { useMetrics } from '../../hooks/useMetrics'
import { useChartTheme } from '../../hooks/useChartTheme'
import { useDashboard } from '../../context/DashboardContext'
import { CHART_LINE_COLORS } from '../../lib/constants'
import { buildNtpAgents, buildNtpSeries } from '../../lib/derive'
import { ChartContainer } from './ChartContainer'
import { RechartsTooltip } from './RechartsTooltip'

export function NtpOffsetChart() {
  const { selectedAgent } = useDashboard()
  const { data, isLoading } = useMetrics({
    agentId: selectedAgent,
    checkType: 'ntp_offset',
    windowSeconds: 3600,
  })
  const chartTheme = useChartTheme()
  const ntpSeries = useMemo(() => buildNtpSeries(data ?? []), [data])
  const ntpAgents = useMemo(() => buildNtpAgents(data ?? []), [data])

  return (
    <ChartContainer
      title="NTP Offset"
      subtitle="Clock skew per agent in ms — zero is perfect"
      isLoading={isLoading}
      isEmpty={ntpAgents.length === 0}
      emptyMessage="No NTP offset data yet. The agent probes ntp_adjtime() each cycle."
      emptyIcon={<Clock className="size-8" aria-hidden="true" />}
    >
      <ResponsiveContainer width="100%" height={240}>
        <LineChart data={ntpSeries} margin={{ top: 10, right: 16, left: 0, bottom: 0 }}>
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
          <Tooltip content={<RechartsTooltip unit=" ms" />} />
          <Legend
            wrapperStyle={{ fontSize: 12, color: chartTheme.axisText }}
            iconType="circle"
            iconSize={8}
          />
          <ReferenceLine y={0} stroke={chartTheme.reference} strokeDasharray="4 4" />
          {ntpAgents.map((agent, i) => (
            <Line
              key={agent}
              type="monotone"
              dataKey={agent}
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
