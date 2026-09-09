import { SchedulesPanel } from '../schedules/SchedulesPanel'
import { RunHistoryPanel } from '../schedules/RunHistoryPanel'

export function SchedulesPage() {
  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-xl font-semibold text-fg">Scheduled Commands</h1>
        <p className="text-base text-fg-muted">
          Run heavy, maintenance commands on selected agents, at a fixed frequency 
          and only inside a limited time window.
        </p>
      </div>
      <SchedulesPanel />
      <div>
        <h2 className="text-base font-semibold text-fg">Recent runs</h2>
        <p className="mt-0.5 mb-3 text-sm text-fg-muted">
          Scheduled and manually-run commands, with their structured results.
        </p>
        <RunHistoryPanel />
      </div>
    </div>
  )
}
