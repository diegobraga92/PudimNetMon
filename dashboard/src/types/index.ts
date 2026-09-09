/** API response shapes shared across the dashboard. */

export interface HealthResponse {
  status: string
  component: string
  storage?: boolean
}

export interface AgentInfo {
  agent_id: string
  last_seen_unix_ms: number
  interval_ms: number
  version: string
  first_seen_unix_ms: number
  diagnostic_endpoint?: string
  alive: boolean
}

export interface AgentsResponse {
  agents: AgentInfo[]
}

export interface MetricPoint {
  time_ms: number
  agent_id: string
  check_type: string
  target: string
  success: boolean
  value: number
  attributes?: Record<string, string>
}

export interface DiagnosticResult {
  success: boolean
  timestamp_unix_ms: number
  result: string
  error?: string
}

// Pre-set, whitelisted commands an agent exposes (never arbitrary shell input).
export interface AgentCommand {
  command_id: string
  description: string
  param_names: string[]
}

export interface AgentCommandsResponse {
  success: boolean
  error?: string
  commands: AgentCommand[]
}

export interface CommandResult {
  success: boolean
  command_id: string
  error?: string
  timestamp_unix_ms: number
  summary?: string
  fields?: Record<string, string>
  issues?: string[]
  detail?: string
}

export interface AgentConfigResponse {
  success: boolean
  applied: string
  error: string
}

// Staged agent binaries the collector serves for the self-hosted download flow.
export interface AgentPlatform {
  id: string          // e.g. "linux-amd64"
  os: string          // "linux" | "windows" | ...
  arch: string        // human-readable, e.g. "x86_64" | "aarch64"
  filename: string    // e.g. "pudim-agent-linux-amd64"
  size_bytes: number
  sha256: string
  download_url: string
}

export interface AgentVersionsResponse {
  version: string
  platforms: AgentPlatform[]
}

// Single-file installer artifacts the collector serves for one-click installs.
export interface AgentInstaller {
  id: string          // e.g. "linux-amd64"
  os: string          // "linux" | "windows"
  arch: string        // human-readable, e.g. "x86_64" | "aarch64"
  kind: string        // "setup" (Inno exe) | "run" (self-extracting .run)
  filename: string    // canonical staged name, e.g. "PudimNetMon-Agent-Setup-0.1.0.exe"
  size_bytes: number
  sha256: string
  download_url: string
}

export interface InstallerVersionsResponse {
  version: string
  installers: AgentInstaller[]
}

export interface ActiveAlert {
  rule_id: string
  rule_name: string
  severity: string
  agent_id: string
  target: string
  value: number
  threshold: number
  acknowledged: boolean
  fired_ms: number
}

export interface AlertHistoryEntry {
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

export type CheckType =
  | 'dns_resolution'
  | 'tcp_connect'
  | 'tls_handshake'
  | 'http_request'
  | 'icmp_ping'
  | 'jitter'
  | 'tls_certificate'
  | 'tcp_retransmit'
  | 'dns_record'
  | 'tcp_handshake'
  | 'ntp_offset'

export type CheckTypeFilter = 'all' | CheckType
export type Severity = 'info' | 'warning' | 'critical'
export type SeverityFilter = 'all' | Severity
export type SeveritySelectValue = Severity | ''

/** Comparison operator for a threshold rule. */
export type AlertOp = '>' | '<'

/** One alert rule as returned by GET /api/alert-rules. */
export interface AlertRule {
  id: string
  name: string
  /** Empty = matches all agents. */
  agent_id: string
  /** Empty = matches all check types. */
  check_type: string
  /** Empty = matches all targets. */
  target: string
  /** Metric field compared against the threshold (unused when on_failure). */
  metric: string
  op: AlertOp
  threshold: number
  /** Min seconds between repeat notifications while the rule is firing. */
  repeat_interval_sec: number
  severity: Severity
  /** Fires when the probe itself fails instead of on a metric threshold. */
  on_failure: boolean
}

export interface AlertRulesResponse {
  success?: boolean
  error?: string
  /** False when the collector has no rules file to persist edits to. */
  persisted?: boolean
  rules: AlertRule[]
}

/** Form state for the agent reconfiguration panel. */
export interface AgentConfigForm {
  dns: string
  tcp: string
  tls: string
  http: string
  ping: string
  pingCount: string
  pingGap: string
  tlsCert: boolean
  tcpRetransmit: boolean
  tcpHandshake: boolean
  httpProtocols: string
}
