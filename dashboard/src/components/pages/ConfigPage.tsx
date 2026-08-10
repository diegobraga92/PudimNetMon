import { AgentConfigPanel } from '../agents/AgentConfigPanel'

export function ConfigPage() {
  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Agent Configuration</h1>
        <p className="text-base text-fg-muted">Reconfigure probe targets at runtime — no agent restart needed.</p>
      </div>
      <AgentConfigPanel />
    </div>
  )
}
