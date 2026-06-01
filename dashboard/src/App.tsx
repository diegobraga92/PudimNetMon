import { useState, useEffect } from 'react'

interface HealthResponse {
  status: string
  component: string
}

interface AgentInfo {
  agent_id: string
  last_seen_unix_ms: number
  interval_ms: number
  version: string
  first_seen_unix_ms: number
  alive: boolean
}

interface AgentsResponse {
  agents: AgentInfo[]
}

function App() {
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [agents, setAgents] = useState<AgentInfo[]>([])
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    const fetchHealth = async () => {
      try {
        const resp = await fetch('/api/health')
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
        const data: HealthResponse = await resp.json()
        setHealth(data)
        setError(null)
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Connection failed')
        setHealth(null)
      }
    }

    const fetchAgents = async () => {
      try {
        const resp = await fetch('/api/agents')
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
        const data: AgentsResponse = await resp.json()
        setAgents(data.agents)
      } catch {
        // Agents endpoint may not be available, that's fine
      }
    }

    // Initial fetch
    fetchHealth()
    fetchAgents()

    // Poll every 5 seconds
    const interval = setInterval(() => {
      fetchHealth()
      fetchAgents()
    }, 5000)

    return () => clearInterval(interval)
  }, [])

  const formatTime = (unixMs: number): string => {
    const date = new Date(unixMs)
    return date.toLocaleTimeString()
  }

  const getStatusClass = (): string => {
    if (error) return 'status-error'
    if (health?.status === 'ok') return 'status-ok'
    return 'status-loading'
  }

  const getAgentStatusClass = (alive: boolean): string => {
    return alive ? 'agent-alive' : 'agent-dead'
  }

  return (
    <div className="app">
      <header className="header">
        <h1>🍮 PudimNetMon</h1>
        <p className="subtitle">Network Monitoring Dashboard</p>
        <div className={`health-indicator ${getStatusClass()}`}>
          <span className="health-dot"></span>
          <span className="health-text">
            {error
              ? `Disconnected: ${error}`
              : health?.status === 'ok'
              ? 'Connected to Collector'
              : 'Connecting...'}
          </span>
        </div>
      </header>

      <main className="main">
        <section className="agents-section">
          <h2>Registered Agents ({agents.length})</h2>
          {agents.length === 0 ? (
            <p className="no-agents">No agents registered yet. Start an agent to see it here.</p>
          ) : (
            <div className="agents-grid">
              {agents.map((agent) => (
                <div key={agent.agent_id} className={`agent-card ${getAgentStatusClass(agent.alive)}`}>
                  <div className="agent-header">
                    <span className="agent-status-dot"></span>
                    <strong>{agent.agent_id}</strong>
                  </div>
                  <div className="agent-details">
                    <p>Last seen: {formatTime(agent.last_seen_unix_ms)}</p>
                    <p>Interval: {agent.interval_ms}ms</p>
                    <p>Version: {agent.version}</p>
                    <p>Status: {agent.alive ? '🟢 Alive' : '🔴 Offline'}</p>
                  </div>
                </div>
              ))}
            </div>
          )}
        </section>
      </main>

      <footer className="footer">
        <p>PudimNetMon v0.1.0 &mdash; Phase 0 Skeleton</p>
      </footer>
    </div>
  )
}

export default App