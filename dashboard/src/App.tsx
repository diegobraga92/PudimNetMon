import { useState, useEffect, useMemo } from 'react'
import {
  LineChart,
  Line,
  BarChart,
  Bar,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
  ResponsiveContainer,
} from 'recharts'

interface HealthResponse {
  status: string
  component: string
  storage?: boolean
}

interface AgentInfo {
  agent_id: string
  last_seen_unix_ms: number
  interval_ms: number
  version: string
  first_seen_unix_ms: number
  diagnostic_endpoint?: string
  alive: boolean
}

interface AgentsResponse {
  agents: AgentInfo[]
}

interface MetricPoint {
  time_ms: number
  agent_id: string
  check_type: string
  target: string
  success: boolean
  value: number
  attributes?: Record<string, string>
}

interface DiagnosticResult {
  success: boolean
  timestamp_unix_ms: number
  result: string
  error?: string
}

interface ActiveAlert {
  rule_id: string
  rule_name: string
  severity: string
  agent_id: string
  target: string
  value: number
  threshold: number
  fired_ms: number
}

interface AlertHistoryEntry {
  rule_id: string
  rule_name: string
  agent_id: string
  check_type: string
  target: string
  severity: string
  status: 'firing' | 'resolved'
  value: number
  threshold: number
  detail: string
  time_ms: number
}

type CheckTypeFilter = 'all' | 'dns_resolution' | 'tcp_connect' | 'tls_handshake' | 'http_request' | 'icmp_ping' | 'jitter' | 'tls_certificate' | 'tcp_retransmit' | 'dns_record' | 'tcp_handshake'

const CHECK_TYPE_LABELS: Record<string, string> = {
  dns_resolution: 'DNS Resolution (ms)',
  tcp_connect: 'TCP Connect (ms)',
  tls_handshake: 'TLS Handshake (ms)',
  http_request: 'HTTP Request (ms)',
  icmp_ping: 'ICMP RTT (ms)',
  jitter: 'Jitter (ms)',
  tls_certificate: 'TLS Certificate',
  tcp_retransmit: 'TCP Retransmits',
  dns_record: 'DNS Records',
  tcp_handshake: 'TCP Handshake (ms)',
}

const CHECK_TYPE_COLORS: Record<string, string> = {
  dns_resolution: '#4ecdc4',
  tcp_connect: '#45b7d1',
  tls_handshake: '#96ceb4',
  http_request: '#f9ca24',
  icmp_ping: '#ff6b6b',
  jitter: '#a29bfe',
  tls_certificate: '#6c5ce7',
  tcp_retransmit: '#fd79a8',
  dns_record: '#00b894',
  tcp_handshake: '#e17055',
}

const SEVERITY_COLORS: Record<string, string> = {
  info: '#4ecdc4',
  warning: '#f9ca24',
  critical: '#ff6b6b',
}

function App() {
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [agents, setAgents] = useState<AgentInfo[]>([])
  const [metrics, setMetrics] = useState<MetricPoint[]>([])
  const [alerts, setAlerts] = useState<ActiveAlert[]>([])
  const [alertHistory, setAlertHistory] = useState<AlertHistoryEntry[]>([])
  const [diagnostics, setDiagnostics] = useState<Record<string, DiagnosticResult>>({})
  const [runningDiag, setRunningDiag] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [selectedAgent, setSelectedAgent] = useState<string>('all')
  const [selectedCheck, setSelectedCheck] = useState<CheckTypeFilter>('all')
  const [windowSeconds, setWindowSeconds] = useState(300)

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

    const fetchMetrics = async () => {
      try {
        const params = new URLSearchParams()
        if (selectedAgent !== 'all') params.set('agent_id', selectedAgent)
        if (selectedCheck !== 'all') params.set('check_type', selectedCheck)
        params.set('window_seconds', String(windowSeconds))

        const resp = await fetch(`/api/metrics?${params.toString()}`)
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`)
        const data: MetricPoint[] = await resp.json()
        setMetrics(data)
      } catch {
        // Metrics may not be available if storage is down
        setMetrics([])
      }
    }

    const fetchAlerts = async () => {
      try {
        const resp = await fetch('/api/alerts')
        if (resp.ok) {
          setAlerts(await resp.json())
        } else {
          setAlerts([])
        }
      } catch {
        setAlerts([])
      }
    }

    const fetchAlertHistory = async () => {
      try {
        const resp = await fetch('/api/alert-history?limit=50')
        if (resp.ok) {
          setAlertHistory(await resp.json())
        } else {
          setAlertHistory([])
        }
      } catch {
        setAlertHistory([])
      }
    }

    fetchHealth()
    fetchAgents()
    fetchMetrics()
    fetchAlerts()
    fetchAlertHistory()

    const interval = setInterval(() => {
      fetchHealth()
      fetchAgents()
      fetchMetrics()
      fetchAlerts()
      fetchAlertHistory()
    }, 5000)

    return () => clearInterval(interval)
  }, [selectedAgent, selectedCheck, windowSeconds])

  const formatTime = (unixMs: number): string => {
    const date = new Date(unixMs)
    return date.toLocaleTimeString()
  }

  const getStatusClass = (): string => {
    if (error) return 'status-error'
    if (health?.status === 'ok') return 'status-ok'
    if (health?.status === 'degraded') return 'status-warn'
    return 'status-loading'
  }

  const getStatusText = (): string => {
    if (error) return `Disconnected: ${error}`
    if (health?.status === 'ok') return 'Connected to Collector'
    if (health?.status === 'degraded') return `Collector Degraded (storage: ${health?.storage ? 'ok' : 'down'})`
    return 'Connecting...'
  }

  const getAgentStatusClass = (alive: boolean): string => {
    return alive ? 'agent-alive' : 'agent-dead'
  }

  // Build chart data: group by agent_id + target within the selected check type
  const chartData = useMemo(() => {
    return metrics
      .filter((m) => m.success)
      .map((m) => ({
        time: new Date(m.time_ms).toLocaleTimeString(),
        time_ms: m.time_ms,
        [`${m.agent_id} | ${m.target}`]: m.value,
      }))
  }, [metrics])

  // Build separate chart lines for each agent/target combo
  const chartLines = useMemo(() => {
    const keys = new Set<string>()
    metrics
      .filter((m) => m.success)
      .forEach((m) => keys.add(`${m.agent_id} | ${m.target}`))
    return Array.from(keys)
  }, [metrics])

  // Stats
  const totalMetrics = metrics.length
  const successRate = metrics.length > 0
    ? Math.round((metrics.filter((m) => m.success).length / metrics.length) * 100)
    : 0

  const activeAgents = agents.filter((a) => a.alive).length

  // --- Phase 4: TLS certificate expiry timeline ---
  // Latest successful tls_certificate metric per target.
  const tlsCerts = useMemo(() => {
    const latest = new Map<string, MetricPoint>()
    metrics
      .filter((m) => m.check_type === 'tls_certificate' && m.success)
      .forEach((m) => latest.set(m.target, m))
    return Array.from(latest.entries()).map(([target, m]) => {
      const days = Number(m.attributes?.tls_cert_expiry_days ?? NaN)
      return {
        target,
        agent_id: m.agent_id,
        expiry_days: Number.isFinite(days) ? days : null,
        subject: m.attributes?.tls_cert_subject ?? '',
        issuer: m.attributes?.tls_cert_issuer ?? '',
        hostname_match: m.attributes?.tls_cert_hostname_match ?? 'unknown',
      }
    })
  }, [metrics])

  // --- Phase 4: HTTP protocol comparison ---
  // Latest latency per (base URL, protocol) for http_request metrics whose
  // target ends with ";http1.1"/";http2"/";http3".
  const protocolComparison = useMemo(() => {
    const latest = new Map<string, { url: string; protocol: string; latency: number }>()
    metrics
      .filter((m) => m.check_type === 'http_request' && m.success)
      .forEach((m) => {
        const semi = m.target.indexOf(';http')
        if (semi === -1) return
        const url = m.target.slice(0, semi)
        const protocol = m.target.slice(semi + 1)
        latest.set(`${url}|${protocol}`, { url, protocol, latency: m.value })
      })
    const groups = new Map<string, { url: string; http11?: number; http2?: number; http3?: number }>()
    latest.forEach((v) => {
      const g = groups.get(v.url) ?? { url: v.url }
      if (v.protocol === 'http1.1') g.http11 = v.latency
      if (v.protocol === 'http2') g.http2 = v.latency
      if (v.protocol === 'http3') g.http3 = v.latency
      groups.set(v.url, g)
    })
    return Array.from(groups.values())
  }, [metrics])

  const runDiagnostic = async (agent: AgentInfo) => {
    if (!agent.diagnostic_endpoint) return
    setRunningDiag(agent.agent_id)
    try {
      const resp = await fetch('/api/diagnostic', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({
          agent_id: agent.agent_id,
          trace_target: 'example.com',
          pcap_duration_s: '5',
          pcap_filter: 'tcp port 443',
        }),
      })
      const data: DiagnosticResult = await resp.json()
      setDiagnostics((prev) => ({ ...prev, [agent.agent_id]: data }))
    } catch (err) {
      setDiagnostics((prev) => ({
        ...prev,
        [agent.agent_id]: {
          success: false,
          timestamp_unix_ms: Date.now(),
          result: '',
          error: err instanceof Error ? err.message : 'diagnostic failed',
        },
      }))
    } finally {
      setRunningDiag(null)
    }
  }

  return (
    <div className="app">
      <header className="header">
        <h1>🍮 PudimNetMon</h1>
        <p className="subtitle">Network Monitoring Dashboard</p>
        <div className={`health-indicator ${getStatusClass()}`}>
          <span className="health-dot"></span>
          <span className="health-text">{getStatusText()}</span>
        </div>
      </header>

      <main className="main">
        <section className="stats-row">
          <div className="stat-card">
            <span className="stat-label">Agents</span>
            <span className="stat-value">{activeAgents}/{agents.length || 0}</span>
            <span className="stat-hint">active</span>
          </div>
          <div className="stat-card">
            <span className="stat-label">Metrics</span>
            <span className="stat-value">{totalMetrics}</span>
            <span className="stat-hint">in window</span>
          </div>
          <div className="stat-card">
            <span className="stat-label">Success</span>
            <span className="stat-value">{successRate}%</span>
            <span className="stat-hint">last 5 min</span>
          </div>
          <div className="stat-card stat-alerts">
            <span className="stat-label">Active Alerts</span>
            <span className={`stat-value ${alerts.length > 0 ? 'stat-value-alert' : ''}`}>{alerts.length}</span>
            <span className="stat-hint">firing now</span>
          </div>
        </section>

        <section className="alerts-section">
          <h2>
            Active Alerts
            {alerts.length > 0 && <span className="alert-badge">{alerts.length} firing</span>}
          </h2>
          {alerts.length === 0 ? (
            <p className="no-alerts">No active alerts. All checks within bounds.</p>
          ) : (
            <div className="alerts-grid">
              {alerts.map((alert) => (
                <div key={`${alert.rule_id}-${alert.agent_id}-${alert.target}`} className="alert-card">
                  <div
                    className="alert-severity-bar"
                    style={{ backgroundColor: SEVERITY_COLORS[alert.severity] ?? '#f9ca24' }}
                  ></div>
                  <div className="alert-card-body">
                    <div className="alert-header">
                      <strong>{alert.rule_name}</strong>
                      <span className={`alert-severity severity-${alert.severity}`}>{alert.severity}</span>
                    </div>
                    <p className="alert-meta">
                      {alert.agent_id} → {alert.target || 'all targets'}
                    </p>
                    <p className="alert-detail">
                      value <strong>{alert.value}</strong> &gt; threshold <strong>{alert.threshold}</strong>
                    </p>
                    <p className="alert-time">Fired {formatTime(alert.fired_ms)}</p>
                  </div>
                </div>
              ))}
            </div>
          )}
        </section>

        <section className="controls">
          <label>
            Agent
            <select value={selectedAgent} onChange={(e) => setSelectedAgent(e.target.value)}>
              <option value="all">All Agents</option>
              {agents.map((a) => (
                <option key={a.agent_id} value={a.agent_id}>{a.agent_id}</option>
              ))}
            </select>
          </label>

          <label>
            Check Type
            <select value={selectedCheck} onChange={(e) => setSelectedCheck(e.target.value as CheckTypeFilter)}>
              <option value="all">All Checks</option>
              {Object.entries(CHECK_TYPE_LABELS).map(([key, label]) => (
                <option key={key} value={key}>{label}</option>
              ))}
            </select>
          </label>

          <label>
            Window
            <select value={windowSeconds} onChange={(e) => setWindowSeconds(Number(e.target.value))}>
              <option value={60}>1 min</option>
              <option value={300}>5 min</option>
              <option value={900}>15 min</option>
              <option value={3600}>1 hour</option>
            </select>
          </label>
        </section>

        <section className="chart-section">
          <h2>
            Time-Series Metrics
            <span className="chart-subtitle">
              {selectedCheck === 'all'
                ? 'All check types (ms)'
                : CHECK_TYPE_LABELS[selectedCheck]}
            </span>
          </h2>
          {chartData.length === 0 ? (
            <p className="no-data">No metric data available. Make sure an agent is running with probes configured.</p>
          ) : (
            <ResponsiveContainer width="100%" height={320}>
              <LineChart data={chartData} margin={{ top: 10, right: 30, left: 0, bottom: 5 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#2d3436" />
                <XAxis dataKey="time" tick={{ fill: '#dfe6e9', fontSize: 12 }} />
                <YAxis tick={{ fill: '#dfe6e9', fontSize: 12 }} />
                <Tooltip
                  contentStyle={{
                    backgroundColor: '#2d3436',
                    border: '1px solid #636e72',
                    borderRadius: 6,
                    color: '#dfe6e9',
                  }}
                />
                <Legend wrapperStyle={{ color: '#dfe6e9' }} />
                {chartLines.map((line, i) => {
                  const color = Object.values(CHECK_TYPE_COLORS)[i % Object.values(CHECK_TYPE_COLORS).length]
                  return (
                    <Line
                      key={line}
                      type="monotone"
                      dataKey={line}
                      stroke={color}
                      strokeWidth={2}
                      dot={false}
                      isAnimationActive={false}
                    />
                  )
                })}
              </LineChart>
            </ResponsiveContainer>
          )}
        </section>

        <section className="tls-section">
          <h2>TLS Certificate Expiry Timeline</h2>
          {tlsCerts.length === 0 ? (
            <p className="no-alerts">No TLS certificate data yet. Configure TLS targets on an agent.</p>
          ) : (
            <div className="tls-grid">
              {tlsCerts.map((cert) => {
                const days = cert.expiry_days
                const cls = days === null ? 'unknown' : days < 7 ? 'critical' : days < 30 ? 'warn' : 'ok'
                return (
                  <div key={cert.target} className={`tls-card tls-${cls}`}>
                    <div className="tls-header">
                      <strong>{cert.target}</strong>
                      <span className="tls-status">
                        {days === null ? 'n/a' : days < 0 ? `${Math.abs(days)}d EXPIRED` : `${days}d left`}
                      </span>
                    </div>
                    <p className="tls-meta">{cert.agent_id} · {cert.subject}</p>
                    <p className="tls-meta">issuer: {cert.issuer}</p>
                    <p className="tls-meta">hostname match: {cert.hostname_match}</p>
                  </div>
                )
              })}
            </div>
          )}
        </section>

        <section className="proto-section">
          <h2>HTTP Protocol Comparison (ms)</h2>
          {protocolComparison.length === 0 ? (
            <p className="no-alerts">No per-protocol data yet. Run the agent with --http-protocols=http1.1,http2,http3</p>
          ) : (
            <ResponsiveContainer width="100%" height={260}>
              <BarChart data={protocolComparison} margin={{ top: 10, right: 30, left: 0, bottom: 5 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#2d3436" />
                <XAxis dataKey="url" tick={{ fill: '#dfe6e9', fontSize: 11 }} />
                <YAxis tick={{ fill: '#dfe6e9', fontSize: 12 }} />
                <Tooltip contentStyle={{ backgroundColor: '#2d3436', border: '1px solid #636e72', borderRadius: 6, color: '#dfe6e9' }} />
                <Legend wrapperStyle={{ color: '#dfe6e9' }} />
                <Bar dataKey="http11" name="HTTP/1.1" fill="#45b7d1" />
                <Bar dataKey="http2" name="HTTP/2" fill="#4ecdc4" />
                <Bar dataKey="http3" name="HTTP/3" fill="#a29bfe" />
              </BarChart>
            </ResponsiveContainer>
          )}
        </section>

        <section className="agents-section">
          <h2>Registered Agents ({agents.length})</h2>
          {agents.length === 0 ? (
            <p className="no-agents">No agents registered yet. Start an agent to see it here.</p>
          ) : (
            <div className="agents-grid">
              {agents.map((agent) => (
                <div
                  key={agent.agent_id}
                  className={`agent-card ${getAgentStatusClass(agent.alive)}`}
                  onClick={() => setSelectedAgent(agent.agent_id)}
                  style={{ cursor: 'pointer' }}
                  title="Click to filter metrics for this agent"
                >
                  <div className="agent-header">
                    <span className="agent-status-dot"></span>
                    <strong>{agent.agent_id}</strong>
                  </div>
                  <div className="agent-details">
                    <p>Last seen: {formatTime(agent.last_seen_unix_ms)}</p>
                    <p>Interval: {agent.interval_ms}ms</p>
                    <p>Version: {agent.version}</p>
                    <p>Status: {agent.alive ? '🟢 Alive' : '🔴 Offline'}</p>
                    {agent.diagnostic_endpoint && (
                      <button
                        className="diag-button"
                        onClick={(e) => {
                          e.stopPropagation()
                          runDiagnostic(agent)
                        }}
                        disabled={runningDiag === agent.agent_id}
                      >
                        {runningDiag === agent.agent_id ? 'Running…' : '🔬 Run Diagnostic'}
                      </button>
                    )}
                  </div>
                </div>
              ))}
            </div>
          )}
          {Object.entries(diagnostics).map(([agentId, diag]) => (
            <div key={agentId} className={`diag-result ${diag.success ? 'ok' : 'fail'}`}>
              <strong>Diagnostic · {agentId}</strong>
              {diag.error && <p className="diag-error">{diag.error}</p>}
              {diag.result && <pre className="diag-pre">{diag.result}</pre>}
            </div>
          ))}
        </section>

        <section className="history-section">
          <h2>Alert History</h2>
          {alertHistory.length === 0 ? (
            <p className="no-alerts">No alert events yet.</p>
          ) : (
            <div className="history-list">
              {alertHistory.slice(0, 30).map((entry, idx) => (
                <div key={idx} className={`history-entry ${entry.status}`}>
                  <span
                    className="history-dot"
                    style={{ backgroundColor: entry.status === 'firing' ? '#ff6b6b' : '#4ecdc4' }}
                  ></span>
                  <div className="history-body">
                    <div className="history-header">
                      <strong>{entry.rule_name}</strong>
                      <span className={`severity-${entry.severity}`}>{entry.severity}</span>
                      <span className={`history-status status-${entry.status}`}>
                        {entry.status === 'firing' ? 'FIRING' : 'RESOLVED'}
                      </span>
                    </div>
                    <p className="history-meta">
                      {entry.agent_id} → {entry.target || 'all targets'} · {entry.check_type}
                    </p>
                    <p className="history-detail">
                      value <strong>{entry.value}</strong> (threshold {entry.threshold})
                      {entry.detail ? ` · ${entry.detail}` : ''}
                    </p>
                  </div>
                  <span className="history-time">{formatTime(entry.time_ms)}</span>
                </div>
              ))}
            </div>
          )}
        </section>
      </main>

      <footer className="footer">
        <p>PudimNetMon v0.2.0 &mdash; Phase 2 Alerting &amp; Notification</p>
      </footer>
    </div>
  )
}

export default App