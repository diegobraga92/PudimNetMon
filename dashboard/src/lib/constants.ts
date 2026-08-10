import type { CheckTypeFilter } from '../types'

export const CHECK_TYPE_LABELS: Record<string, string> = {
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
  ntp_offset: 'NTP Offset (ms)',
}

export const CHECK_TYPE_COLORS: Record<string, string> = {
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
  ntp_offset: '#fdcb6e',
}

/** Chart-line colors in a stable order (indexed, not keyed). */
export const CHART_LINE_COLORS = Object.values(CHECK_TYPE_COLORS)

export const SEVERITY_COLORS: Record<string, string> = {
  info: '#4ecdc4',
  warning: '#f9ca24',
  critical: '#ff6b6b',
}

export const SEVERITY_ORDER = ['critical', 'warning', 'info'] as const

export const TIME_WINDOWS = [
  { value: 60, label: '1 min' },
  { value: 300, label: '5 min' },
  { value: 900, label: '15 min' },
  { value: 3600, label: '1 hour' },
  { value: 21600, label: '6 hours' },
  { value: 86400, label: '24 hours' },
]

export const CHECK_TYPE_OPTIONS: { value: CheckTypeFilter; label: string }[] = [
  { value: 'all', label: 'All Checks' },
  ...Object.entries(CHECK_TYPE_LABELS).map(([key, label]) => ({
    value: key as CheckTypeFilter,
    label,
  })),
]

/** Data polling cadence for live views (ms). */
export const POLL_INTERVAL_MS = 5000

/** Human-readable labels for the sidebar views (used in the header context). */
export const VIEW_LABELS: Record<string, string> = {
  overview: 'Overview',
  metrics: 'Metrics',
  agents: 'Agents',
  alerts: 'Alerts',
  history: 'Alert History',
  config: 'Agent Config',
}
