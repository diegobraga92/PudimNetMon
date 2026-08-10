import { AlertHistoryPanel } from '../alerts/AlertHistoryPanel'

export function HistoryPage() {
  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Alert History</h1>
        <p className="text-base text-fg-muted">Every alert event — fired and resolved — across all agents.</p>
      </div>
      <AlertHistoryPanel />
    </div>
  )
}
