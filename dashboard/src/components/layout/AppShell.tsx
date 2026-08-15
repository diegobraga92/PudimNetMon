import { useEffect, useMemo } from 'react'
import { useDashboard } from '../../context/DashboardContext'
import { useAgents } from '../../hooks/useAgents'
import { useAlerts } from '../../hooks/useAlerts'
import { useHealth } from '../../hooks/useHealth'
import { ErrorBoundary } from '../ui/ErrorBoundary'
import { Header } from './Header'
import { Sidebar } from './Sidebar'
import { OverviewPage } from '../pages/OverviewPage'
import { MetricsPage } from '../pages/MetricsPage'
import { AgentsPage } from '../pages/AgentsPage'
import { AlertsPage } from '../pages/AlertsPage'
import { HistoryPage } from '../pages/HistoryPage'
import { ConfigPage } from '../pages/ConfigPage'
import { DeployPage } from '../pages/DeployPage'

export function AppShell() {
  const { view, sidebarOpen, setSidebarOpen } = useDashboard()
  const health = useHealth()
  const alerts = useAlerts()
  const agents = useAgents()

  // Reflect unacknowledged firing alerts in the browser tab title.
  useEffect(() => {
    const firing = alerts.data?.filter((a) => !a.acknowledged).length ?? 0
    document.title =
      firing > 0
        ? `(${firing}) PudimNetMon — Network Monitoring`
        : 'PudimNetMon — Network Monitoring'
  }, [alerts.data])

  /** Latest successful fetch across the live queries — drives the "Updated Xs ago" pill. */
  const lastUpdated = useMemo(() => {
    const ts = Math.max(
      health.dataUpdatedAt,
      alerts.dataUpdatedAt,
      agents.dataUpdatedAt,
    )
    return ts > 0 ? ts : null
  }, [health.dataUpdatedAt, alerts.dataUpdatedAt, agents.dataUpdatedAt])

  return (
    <div className="flex min-h-screen bg-bg text-fg">
      <Sidebar open={sidebarOpen} onClose={() => setSidebarOpen(false)} />
      <div className="flex min-w-0 flex-1 flex-col">
        <Header lastUpdated={lastUpdated} onOpenSidebar={() => setSidebarOpen(true)} />
        <main className="mx-auto w-full max-w-7xl flex-1 p-4 md:p-6">
          <ErrorBoundary>
            <div key={view} className="animate-view-enter">
              {view === 'overview' && <OverviewPage />}
              {view === 'metrics' && <MetricsPage />}
              {view === 'agents' && <AgentsPage />}
              {view === 'alerts' && <AlertsPage />}
              {view === 'history' && <HistoryPage />}
              {view === 'config' && <ConfigPage />}
              {view === 'deploy' && <DeployPage />}
            </div>
          </ErrorBoundary>
        </main>
      </div>
    </div>
  )
}
