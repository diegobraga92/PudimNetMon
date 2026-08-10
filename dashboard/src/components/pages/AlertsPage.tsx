import { AlertsPanel } from '../alerts/AlertsPanel'

export function AlertsPage() {
  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Active Alerts</h1>
        <p className="text-base text-fg-muted">Acknowledge alerts to take ownership. They refresh automatically.</p>
      </div>
      <AlertsPanel />
    </div>
  )
}
