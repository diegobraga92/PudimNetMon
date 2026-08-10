import { Activity, AlertOctagon, Gauge, Radio } from 'lucide-react'
import { useDashboard } from '../../context/DashboardContext'
import { useAgents } from '../../hooks/useAgents'
import { useAlerts } from '../../hooks/useAlerts'
import { useMetrics } from '../../hooks/useMetrics'
import { StatCard } from '../ui/StatCard'
import { FilterBar } from '../ui/FilterBar'
import { Card } from '../ui/Card'
import { AlertsPanel } from '../alerts/AlertsPanel'
import { MetricsChart } from '../charts/MetricsChart'
import { TlsCertsGrid } from '../charts/TlsCertsGrid'
import { HttpDistribution } from '../charts/HttpDistribution'
import { NtpOffsetChart } from '../charts/NtpOffsetChart'

export function OverviewPage() {
  const { setView, selectedAgent, selectedCheck, windowSeconds } = useDashboard()

  const agentsQuery = useAgents()
  const alertsQuery = useAlerts()
  const metricsQuery = useMetrics({ agentId: selectedAgent, checkType: selectedCheck, windowSeconds })

  const agents = agentsQuery.data?.agents ?? []
  const alerts = alertsQuery.data ?? []
  const metrics = metricsQuery.data ?? []

  const activeAgents = agents.filter((a) => a.alive).length
  const successCount = metrics.filter((m) => m.success).length
  const successRate = metrics.length > 0 ? Math.round((successCount / metrics.length) * 100) : 0
  const firingCount = alerts.filter((a) => !a.acknowledged).length

  return (
    <div className="space-y-5">
      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <StatCard
          label="Agents"
          value={`${activeAgents}/${agents.length || 0}`}
          hint="active"
          icon={<Radio className="size-4" aria-hidden="true" />}
          loading={agentsQuery.isLoading}
          onClick={() => setView('agents')}
        />
        <StatCard
          label="Metrics"
          value={metrics.length}
          hint="in window"
          icon={<Activity className="size-4" aria-hidden="true" />}
          loading={metricsQuery.isLoading}
        />
        <StatCard
          label="Success"
          value={metrics.length > 0 ? `${successRate}%` : '—'}
          hint="last window"
          icon={<Gauge className="size-4" aria-hidden="true" />}
          loading={metricsQuery.isLoading}
          tone={
            metrics.length === 0 ? 'default' : successRate >= 98 ? 'success' : successRate >= 90 ? 'warning' : 'critical'
          }
        />
        <StatCard
          label="Firing Alerts"
          value={firingCount}
          hint={alerts.length > 0 ? `${alerts.length} total` : 'all clear'}
          icon={<AlertOctagon className="size-4" aria-hidden="true" />}
          loading={alertsQuery.isLoading}
          tone={firingCount > 0 ? 'critical' : 'success'}
          onClick={() => setView('alerts')}
        />
      </div>

      <AlertsPanel compact />

      <Card className="p-3">
        <FilterBar />
      </Card>

      <MetricsChart />

      <div className="grid gap-5 xl:grid-cols-2">
        <TlsCertsGrid />
        <HttpDistribution />
      </div>

      <NtpOffsetChart />
    </div>
  )
}
