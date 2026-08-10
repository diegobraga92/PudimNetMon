import type { MetricPoint } from '../types'

/** A recharts-friendly data point keyed by series label. */
export interface SeriesPoint {
  time: string
  time_ms: number
  [key: string]: string | number
}

/** Group successful metrics into per (agent, target) series for the line chart. */
export function buildChartData(metrics: MetricPoint[]): SeriesPoint[] {
  return metrics
    .filter((m) => m.success)
    .map((m) => ({
      time: new Date(m.time_ms).toLocaleTimeString(),
      time_ms: m.time_ms,
      [`${m.agent_id} | ${m.target}`]: m.value,
    }))
}

export function buildChartLines(metrics: MetricPoint[]): string[] {
  const keys = new Set<string>()
  metrics.filter((m) => m.success).forEach((m) => keys.add(`${m.agent_id} | ${m.target}`))
  return Array.from(keys)
}

export interface TlsCertInfo {
  target: string
  agent_id: string
  expiry_days: number | null
  subject: string
  issuer: string
  hostname_match: string
}

/** Latest successful tls_certificate metric per target. */
export function buildTlsCerts(metrics: MetricPoint[]): TlsCertInfo[] {
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
}

export interface ProtocolComparison {
  url: string
  http11?: number
  http2?: number
  http3?: number
}

/** Latest latency per (base URL, protocol) for `target;http1.1` style targets. */
export function buildProtocolComparison(metrics: MetricPoint[]): ProtocolComparison[] {
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
  const groups = new Map<string, ProtocolComparison>()
  latest.forEach((v) => {
    const g = groups.get(v.url) ?? { url: v.url }
    if (v.protocol === 'http1.1') g.http11 = v.latency
    if (v.protocol === 'http2') g.http2 = v.latency
    if (v.protocol === 'http3') g.http3 = v.latency
    groups.set(v.url, g)
  })
  return Array.from(groups.values())
}

/** NTP offset time series, one line per agent. */
export function buildNtpSeries(metrics: MetricPoint[]): SeriesPoint[] {
  return metrics
    .filter((m) => m.check_type === 'ntp_offset' && m.success)
    .map((m) => ({
      time: new Date(m.time_ms).toLocaleTimeString(),
      time_ms: m.time_ms,
      [m.agent_id]: m.value,
    }))
}

export function buildNtpAgents(metrics: MetricPoint[]): string[] {
  const keys = new Set<string>()
  metrics
    .filter((m) => m.check_type === 'ntp_offset' && m.success)
    .forEach((m) => keys.add(m.agent_id))
  return Array.from(keys)
}
