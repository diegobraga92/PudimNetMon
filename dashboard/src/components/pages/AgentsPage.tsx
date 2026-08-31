import { useState } from 'react'
import type { AgentInfo } from '../../types'
import { AgentsPanel } from '../agents/AgentsPanel'
import { DiagnosticDialog } from '../agents/DiagnosticDialog'
import { CommandDialog } from '../agents/CommandDialog'

export function AgentsPage() {
  const [diagAgent, setDiagAgent] = useState<AgentInfo | null>(null)
  const [dialogOpen, setDialogOpen] = useState(false)
  const [cmdAgent, setCmdAgent] = useState<AgentInfo | null>(null)
  const [cmdOpen, setCmdOpen] = useState(false)

  const handleRunDiagnostic = (agent: AgentInfo) => {
    setDiagAgent(agent)
    setDialogOpen(true)
  }

  const handleRunCommand = (agent: AgentInfo) => {
    setCmdAgent(agent)
    setCmdOpen(true)
  }

  return (
    <div className="space-y-5">
      <div>
        <h1 className="text-xl font-semibold text-fg">Agents</h1>
        <p className="text-base text-fg-muted">
          Every connected probe daemon, its liveness and recent latency.
        </p>
      </div>
      <AgentsPanel onRunDiagnostic={handleRunDiagnostic} onRunCommand={handleRunCommand} />
      <DiagnosticDialog
        agent={diagAgent}
        open={dialogOpen}
        onOpenChange={setDialogOpen}
        onResult={() => {}}
      />
      <CommandDialog agent={cmdAgent} open={cmdOpen} onOpenChange={setCmdOpen} />
    </div>
  )
}
