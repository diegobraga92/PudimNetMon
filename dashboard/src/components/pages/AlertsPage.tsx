import { AlertsPanel } from '../alerts/AlertsPanel'
import { AlertRulesPanel } from '../alerts/AlertRulesPanel'

export function AlertsPage() {
  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-xl font-semibold text-fg">Active Alerts</h1>
        <p className="text-base text-fg-muted">Acknowledge alerts to take ownership. They refresh automatically.</p>
      </div>
      <AlertsPanel />
      <AlertRulesPanel />
    </div>
  )
}
