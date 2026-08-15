import { vi } from 'vitest'
import type {
  AgentInfo,
  AgentVersionsResponse,
  AlertHistoryEntry,
  HealthResponse,
  MetricPoint,
  ActiveAlert,
} from '../types'

export const mockHealth: HealthResponse = {
  status: 'ok',
  component: 'collector',
  storage: true,
}

export const mockAgentVersions: AgentVersionsResponse = {
  version: '0.1.0',
  platforms: [
    {
      id: 'linux-amd64',
      os: 'linux',
      arch: 'x86_64',
      filename: 'pudim-agent-linux-amd64',
      size_bytes: 1500000,
      sha256: 'a'.repeat(64),
      download_url: '/api/agent/download?platform=linux-amd64',
    },
    {
      id: 'windows-amd64',
      os: 'windows',
      arch: 'x86_64',
      filename: 'pudim-agent-windows-amd64.exe',
      size_bytes: 2200000,
      sha256: 'b'.repeat(64),
      download_url: '/api/agent/download?platform=windows-amd64',
    },
  ],
}

export const mockAgents: AgentInfo[] = [
  {
    agent_id: 'agent-1',
    last_seen_unix_ms: Date.now() - 2000,
    interval_ms: 1000,
    version: '0.2.0',
    first_seen_unix_ms: Date.now() - 86400000,
    alive: true,
    diagnostic_endpoint: 'localhost:50052',
  },
  {
    agent_id: 'agent-2',
    last_seen_unix_ms: Date.now() - 900000,
    interval_ms: 1000,
    version: '0.2.0',
    first_seen_unix_ms: Date.now() - 3600000,
    alive: false,
  },
  {
    agent_id: 'agent-3',
    last_seen_unix_ms: Date.now() - 1000,
    interval_ms: 2000,
    version: '0.2.0',
    first_seen_unix_ms: Date.now() - 7200000,
    alive: true,
  },
]

const now = Date.now()
export const mockMetrics: MetricPoint[] = [
  {
    time_ms: now - 4000,
    agent_id: 'agent-1',
    check_type: 'icmp_ping',
    target: 'example.com',
    success: true,
    value: 12.4,
  },
  {
    time_ms: now - 4000,
    agent_id: 'agent-3',
    check_type: 'icmp_ping',
    target: 'example.com',
    success: true,
    value: 8.1,
  },
  {
    time_ms: now - 4000,
    agent_id: 'agent-1',
    check_type: 'icmp_ping',
    target: 'google.com',
    success: true,
    value: 15.7,
  },
  {
    time_ms: now - 4000,
    agent_id: 'agent-1',
    check_type: 'dns_resolution',
    target: 'example.com',
    success: false,
    value: 200,
  },
  {
    time_ms: now - 9000,
    agent_id: 'agent-1',
    check_type: 'ntp_offset',
    target: 'ntp',
    success: true,
    value: 2.1,
  },
  {
    time_ms: now - 9000,
    agent_id: 'agent-3',
    check_type: 'ntp_offset',
    target: 'ntp',
    success: true,
    value: -1.3,
  },
]

export const mockAlerts: ActiveAlert[] = [
  {
    rule_id: 'latency-1',
    rule_name: 'High Latency',
    severity: 'critical',
    agent_id: 'agent-1',
    target: 'example.com',
    value: 250,
    threshold: 200,
    acknowledged: false,
    fired_ms: now - 60000,
  },
  {
    rule_id: 'latency-2',
    rule_name: 'Packet Loss',
    severity: 'warning',
    agent_id: 'agent-2',
    target: 'all targets',
    value: 12,
    threshold: 5,
    acknowledged: true,
    fired_ms: now - 300000,
  },
]

export const mockAlertHistory: AlertHistoryEntry[] = [
  {
    rule_id: 'latency-1',
    rule_name: 'High Latency',
    agent_id: 'agent-1',
    check_type: 'icmp_ping',
    target: 'example.com',
    severity: 'critical',
    status: 'resolved',
    value: 240,
    threshold: 200,
    detail: 'recovered',
    time_ms: now - 1800000,
  },
]

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  })
}

export interface MockApiOptions {
  health?: HealthResponse
  agents?: AgentInfo[]
  metrics?: MetricPoint[]
  alerts?: ActiveAlert[]
  alertHistory?: AlertHistoryEntry[]
  agentVersions?: AgentVersionsResponse
}

/** Stub global.fetch with canned responses routed by URL. Returns the fetch stub. */
export function mockApi(options: MockApiOptions = {}) {
  const {
    health = mockHealth,
    agents = mockAgents,
    metrics = mockMetrics,
    alerts = mockAlerts,
    alertHistory = mockAlertHistory,
    agentVersions = mockAgentVersions,
  } = options

  let currentAlerts = alerts

  const fetchMock = vi.fn(async (input: RequestInfo | URL) => {
    const url = typeof input === 'string' ? input : input.toString()
    if (url.startsWith('/api/health')) return jsonResponse(health)
    if (url.startsWith('/api/agents')) return jsonResponse({ agents })
    if (url.startsWith('/api/metrics')) return jsonResponse(metrics)
    if (url.startsWith('/api/alerts/ack')) {
      const body = await (input instanceof Request ? input.json() : Promise.resolve(null))
      if (body) {
        currentAlerts = currentAlerts.map((a) =>
          a.rule_id === body.rule_id && a.agent_id === body.agent_id ? { ...a, acknowledged: true } : a,
        )
      }
      return jsonResponse({ alerts: currentAlerts })
    }
    if (url.startsWith('/api/alerts')) return jsonResponse(currentAlerts)
    if (url.startsWith('/api/alert-history')) return jsonResponse(alertHistory)
    if (url.startsWith('/api/agent/versions')) return jsonResponse(agentVersions)
    if (url.startsWith('/api/diagnostic')) {
      return jsonResponse({ success: true, timestamp_unix_ms: Date.now(), result: 'traceroute ok\npcap ok' })
    }
    return jsonResponse({ error: 'not found' }, 404)
  })

  vi.stubGlobal('fetch', fetchMock)
  return { fetchMock, getAlerts: () => currentAlerts }
}
